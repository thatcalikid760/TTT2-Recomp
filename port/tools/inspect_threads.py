"""Sample Windows host PCs and stack candidates, briefly pausing each thread.

No process-memory writes. Each successful suspend is paired with ResumeThread
in finally. Stack candidates are a heuristic scan, not an unwound call stack.
"""
import argparse
import bisect
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import struct

from inspect_guest import ReadProcess, parse_map


class ThreadEntry(C.Structure):
    _fields_ = [("size", W.DWORD), ("usage", W.DWORD), ("tid", W.DWORD),
                ("pid", W.DWORD), ("priority", W.LONG), ("delta", W.LONG),
                ("flags", W.DWORD)]


def sample(pid, map_path):
    process = ReadProcess(pid)
    try:
        linker = parse_map(map_path.read_text(encoding="utf-8-sig"))
        process.resolve_guest_base(linker)  # Reject mismatched executable/map.
        module = process.module
        pe_offset = struct.unpack("<I", process.read(module["base"] + 0x3C, 4))[0]
        pe = process.read(module["base"] + pe_offset, 24)
        if pe[:4] != b"PE\0\0":
            raise ValueError("Process module has no PE header")
        section_count, = struct.unpack_from("<H", pe, 6)
        optional_size, = struct.unpack_from("<H", pe, 20)
        sections = process.read(module["base"] + pe_offset + 24 + optional_size, section_count * 40)
        executable_ranges = []
        for offset in range(0, len(sections), 40):
            virtual_size, virtual_address, raw_size = struct.unpack_from("<III", sections, offset + 8)
            characteristics, = struct.unpack_from("<I", sections, offset + 36)
            if characteristics & 0x20000000:
                executable_ranges.append((virtual_address, virtual_address + max(virtual_size, raw_size)))
        symbols = sorted((rva, name) for name, rva in linker["symbols"].items())
        addresses = [rva for rva, _ in symbols]

        def describe(address):
            rva = address - module["base"]
            if not 0 <= rva < module["size"]:
                return {"host": f"0x{address:X}"}
            i = bisect.bisect_right(addresses, rva) - 1
            return {"rva": f"0x{rva:X}", "nearest_symbol": symbols[i][1] if i >= 0 else None,
                    "offset": f"0x{rva-symbols[i][0]:X}" if i >= 0 else None}

        k = process.k32
        for name, args, result in [
            ("OpenThread", [W.DWORD, W.BOOL, W.DWORD], W.HANDLE),
            ("SuspendThread", [W.HANDLE], W.DWORD),
            ("ResumeThread", [W.HANDLE], W.DWORD),
            ("GetThreadContext", [W.HANDLE, C.c_void_p], W.BOOL),
            ("Thread32First", [W.HANDLE, C.POINTER(ThreadEntry)], W.BOOL),
            ("Thread32Next", [W.HANDLE, C.POINTER(ThreadEntry)], W.BOOL),
            ("GetThreadTimes", [W.HANDLE] + [C.POINTER(W.FILETIME)]*4, W.BOOL),
        ]:
            fn = getattr(k, name); fn.argtypes = args; fn.restype = result
        snapshot = k.CreateToolhelp32Snapshot(4, 0)
        if snapshot == C.c_void_p(-1).value:
            raise C.WinError(C.get_last_error())
        captures = []
        try:
            entry = ThreadEntry(); entry.size = C.sizeof(entry)
            ok = k.Thread32First(snapshot, C.byref(entry))
            while ok:
                if entry.pid == pid:
                    handle = k.OpenThread(0x0002 | 0x0008 | 0x0040, False, entry.tid)
                    if handle:
                        suspended = False
                        capture = {"tid": entry.tid}
                        try:
                            times = [W.FILETIME() for _ in range(4)]
                            if k.GetThreadTimes(handle, *(C.byref(t) for t in times)):
                                capture["cpu_seconds"] = sum((t.dwHighDateTime << 32) | t.dwLowDateTime
                                                             for t in times[2:]) / 1e7
                            buffer = C.create_string_buffer(1248)
                            aligned = (C.addressof(buffer) + 15) & ~15
                            C.c_uint32.from_address(aligned + 48).value = 0x00100003
                            if k.SuspendThread(handle) == 0xFFFFFFFF:
                                raise C.WinError(C.get_last_error())
                            suspended = True
                            if not k.GetThreadContext(handle, aligned):
                                raise C.WinError(C.get_last_error())
                            raw = C.string_at(aligned, 1232)
                            rsp = struct.unpack_from("<Q", raw, 152)[0]
                            rip = struct.unpack_from("<Q", raw, 248)[0]
                            capture["pc"] = describe(rip)
                            names = ("rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                     "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15")
                            capture["registers"] = {name: f"0x{struct.unpack_from('<Q', raw, 120+i*8)[0]:X}"
                                                    for i, name in enumerate(names)}
                            # Read page by page, retaining the valid prefix when near
                            # the end of a stack. Data pointers are not return PCs.
                            stack = bytearray()
                            while len(stack) < 8192:
                                address = rsp + len(stack)
                                length = min(4096 - (address & 4095), 8192 - len(stack))
                                try:
                                    stack.extend(process.read(address, length))
                                except OSError as error:
                                    capture["stack_error"] = str(error)
                                    break
                            found = []
                            for offset, (value,) in enumerate(struct.iter_unpack("<Q", stack[:len(stack)//8*8])):
                                rva = value - module["base"]
                                if any(begin <= rva < end for begin, end in executable_ranges):
                                    found.append({"stack_offset": offset*8, **describe(value)})
                                    if len(found) == 40: break
                            capture["stack_candidates"] = found
                        except OSError as error:
                            capture["error"] = str(error)
                        finally:
                            try:
                                if suspended and k.ResumeThread(handle) == 0xFFFFFFFF:
                                    raise C.WinError(C.get_last_error())
                            finally:
                                k.CloseHandle(handle)
                        captures.append(capture)
                ok = k.Thread32Next(snapshot, C.byref(entry))
        finally:
            k.CloseHandle(snapshot)
        return {"pid": pid, "module": module, "observation": "individual thread pauses; heuristic stack candidates",
                "threads": sorted(captures, key=lambda t: t.get("cpu_seconds", 0), reverse=True)}
    finally:
        process.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--map", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(sample(args.pid, args.map), indent=2))
