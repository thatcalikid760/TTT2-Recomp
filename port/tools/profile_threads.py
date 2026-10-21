"""Statistical per-thread host+guest profile of a live tekken_recomp process.

Each round briefly suspends every selected thread, reads RIP/RSP (and, when the
build publishes g_guestContextRegistry, the guest r1/lr of the thread's active
PPCContext), then resumes it. Same access pattern as inspect_threads.py: no
process-memory writes. Reports, per thread:

  * CPU seconds consumed during the window (GetThreadTimes delta),
  * host leaf-symbol histogram,
  * host stack chains for samples outside the main module (heuristic scan of
    return-address candidates - stale frames are possible),
  * guest call stacks (exact PPC backchain walk: lr, then saved LR of each
    frame), and the guest functions most often on the stack (inclusive).

Requires the /MAP from the exact running executable (checked by PE timestamp).

  python port/tools/profile_threads.py --pid 123 --map port/build-o1/tekken_recomp.map --seconds 20
"""
import argparse
import bisect
import collections
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import re
import struct
import time

from inspect_guest import ReadProcess, parse_map


class ThreadEntry(C.Structure):
    _fields_ = [("size", W.DWORD), ("usage", W.DWORD), ("tid", W.DWORD),
                ("pid", W.DWORD), ("priority", W.LONG), ("delta", W.LONG),
                ("flags", W.DWORD)]


class ModuleEntry(C.Structure):
    _fields_ = [("dwSize", W.DWORD), ("th32ModuleID", W.DWORD),
                ("th32ProcessID", W.DWORD), ("GlblcntUsage", W.DWORD),
                ("ProccntUsage", W.DWORD), ("modBaseAddr", C.c_void_p),
                ("modBaseSize", W.DWORD), ("hModule", W.HMODULE),
                ("szModule", W.WCHAR * 256), ("szExePath", W.WCHAR * 260)]


def bind(k):
    for name, args, result in [
        ("OpenThread", [W.DWORD, W.BOOL, W.DWORD], W.HANDLE),
        ("SuspendThread", [W.HANDLE], W.DWORD),
        ("ResumeThread", [W.HANDLE], W.DWORD),
        ("GetThreadContext", [W.HANDLE, C.c_void_p], W.BOOL),
        ("Thread32First", [W.HANDLE, C.POINTER(ThreadEntry)], W.BOOL),
        ("Thread32Next", [W.HANDLE, C.POINTER(ThreadEntry)], W.BOOL),
        ("Module32FirstW", [W.HANDLE, C.POINTER(ModuleEntry)], W.BOOL),
        ("Module32NextW", [W.HANDLE, C.POINTER(ModuleEntry)], W.BOOL),
        ("GetThreadTimes", [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4, W.BOOL),
        ("CreateToolhelp32Snapshot", [W.DWORD, W.DWORD], W.HANDLE),
    ]:
        fn = getattr(k, name); fn.argtypes = args; fn.restype = result


def list_threads(k, pid):
    snapshot = k.CreateToolhelp32Snapshot(4, 0)
    if snapshot == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error())
    tids = []
    try:
        entry = ThreadEntry(); entry.size = C.sizeof(entry)
        ok = k.Thread32First(snapshot, C.byref(entry))
        while ok:
            if entry.pid == pid:
                tids.append(entry.tid)
            ok = k.Thread32Next(snapshot, C.byref(entry))
    finally:
        k.CloseHandle(snapshot)
    return tids


def list_modules(k, pid):
    snapshot = k.CreateToolhelp32Snapshot(0x8 | 0x10, pid)
    if snapshot == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error())
    modules = []
    try:
        entry = ModuleEntry(); entry.dwSize = C.sizeof(entry)
        ok = k.Module32FirstW(snapshot, C.byref(entry))
        while ok:
            modules.append((entry.modBaseAddr, entry.modBaseAddr + entry.modBaseSize, entry.szModule))
            ok = k.Module32NextW(snapshot, C.byref(entry))
    finally:
        k.CloseHandle(snapshot)
    return sorted(modules)


def cpu_seconds(k, handle):
    times = [W.FILETIME() for _ in range(4)]
    if not k.GetThreadTimes(handle, *(C.byref(t) for t in times)):
        return None
    return sum((t.dwHighDateTime << 32) | t.dwLowDateTime for t in times[2:]) / 1e7


class GuestStacks:
    """Reads g_guestContextRegistry (kernel_thread.cpp) and walks PPC backchains."""

    HEADER = struct.Struct("<6I")   # magic, version, r1Offset, lrOffset, capacity, count
    SLOT = struct.Struct("<IIQ")    # tid, entry, PPCContext*

    def __init__(self, process, linker, guest_base, depth):
        self.process, self.guest, self.depth = process, guest_base, depth
        rva = linker["symbols"].get("g_guestContextRegistry")
        self.address = process.module["base"] + rva if rva is not None else None
        self.slots = {}
        starts = sorted({int(m.group(1), 16) for name in linker["symbols"]
                         for m in [re.search(r"sub_([0-9A-F]{8})$", name)] if m})
        self.starts = starts
        if self.address is not None:
            magic, version, self.r1, self.lr, capacity, _ = self.HEADER.unpack(
                process.read(self.address, self.HEADER.size))
            if magic != 0x47435458 or version != 1:
                raise ValueError("g_guestContextRegistry has an unknown layout")
            self.capacity = capacity
            self.refresh()

    @property
    def available(self):
        return self.address is not None

    def refresh(self):
        count = min(self.HEADER.unpack(self.process.read(self.address, self.HEADER.size))[5], self.capacity)
        if not count:
            return
        raw = self.process.read(self.address + 24, count * self.SLOT.size)
        for i in range(count):
            tid, _, _ = self.SLOT.unpack_from(raw, i * self.SLOT.size)
            self.slots[tid] = self.address + 24 + i * self.SLOT.size

    def name(self, address):
        i = bisect.bisect_right(self.starts, address) - 1
        if i < 0 or address - self.starts[i] > 0x10000:
            return f"{address:08X}"
        return f"sub_{self.starts[i]:08X}"

    def capture(self, tid):
        """Raw guest stack for a suspended thread: (entry, lr, r1, bytes) or None."""
        slot = self.slots.get(tid)
        if slot is None:
            return None
        _, entry, ctx = self.SLOT.unpack(self.process.read(slot, self.SLOT.size))
        if not ctx:
            return None
        r1 = struct.unpack("<Q", self.process.read(ctx + self.r1, 8))[0] & 0xFFFFFFFF
        lr = struct.unpack("<Q", self.process.read(ctx + self.lr, 8))[0] & 0xFFFFFFFF
        try:
            data = self.process.read(self.guest + r1, 0x4000)
        except OSError:
            data = b""
        return entry, lr, r1, data

    def walk(self, captured):
        entry, lr, r1, data = captured
        frames = [lr]
        frame = r1
        for _ in range(self.depth):
            off = frame - r1
            if not 0 <= off <= len(data) - 4:
                break
            prev = struct.unpack_from(">I", data, off)[0]
            if not prev or prev <= frame or prev - frame > 0x10000:
                break
            off = prev - 8 - r1
            if not 0 <= off <= len(data) - 4:
                break
            frames.append(struct.unpack_from(">I", data, off)[0])
            frame = prev
        return entry, [self.name(a) for a in frames if 0x82000000 <= a < 0x84000000]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--map", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--hz", type=float, default=100, help="target sampling rounds per second")
    parser.add_argument("--top", type=int, default=12)
    parser.add_argument("--tid", type=int, action="append", help="sample only these thread ids")
    parser.add_argument("--stack-bytes", type=int, default=2048)
    parser.add_argument("--chain-all", action="store_true", help="host stack-scan every sample, not only out-of-module ones")
    parser.add_argument("--depth", type=int, default=3, help="exe code frames kept for host chains")
    parser.add_argument("--guest-depth", type=int, default=8, help="guest frames kept per stack")
    parser.add_argument("--min-cpu", type=float, default=0.02, help="hide threads below this CPU fraction")
    parser.add_argument("--json", type=Path, help="also write the full result here")
    args = parser.parse_args()

    process = ReadProcess(args.pid)
    k = process.k32
    bind(k)
    try:
        linker = parse_map(args.map.read_text(encoding="utf-8-sig"))
        guest_base = process.resolve_guest_base(linker)  # rejects a mismatched executable/map
        base, size = process.module["base"], process.module["size"]
        symbols = sorted((rva, name) for name, rva in linker["symbols"].items())
        addresses = [rva for rva, _ in symbols]
        modules = list_modules(k, args.pid)
        module_starts = [m[0] for m in modules]
        guest = GuestStacks(process, linker, guest_base, args.guest_depth)

        def symbol(address):
            rva = address - base
            if 0 <= rva < size:
                i = bisect.bisect_right(addresses, rva) - 1
                return symbols[i][1] if i >= 0 else f"exe+0x{rva:X}"
            i = bisect.bisect_right(module_starts, address) - 1
            if i >= 0 and address < modules[i][1]:
                return f"{modules[i][2]}+0x{address - modules[i][0]:X}"
            return f"0x{address:X}"

        def in_main(address):
            return 0 <= address - base < size

        # Return-address candidates must point into executable sections; data
        # symbols (e.g. a mutex whose address sits on the stack) are skipped.
        pe_offset = struct.unpack("<I", process.read(base + 0x3C, 4))[0]
        pe = process.read(base + pe_offset, 24)
        section_count, = struct.unpack_from("<H", pe, 6)
        optional_size, = struct.unpack_from("<H", pe, 20)
        sections = process.read(base + pe_offset + 24 + optional_size, section_count * 40)
        code_ranges = []
        for offset in range(0, len(sections), 40):
            virtual_size, virtual_address = struct.unpack_from("<II", sections, offset + 8)
            if struct.unpack_from("<I", sections, offset + 36)[0] & 0x20000000:
                code_ranges.append((base + virtual_address, base + virtual_address + virtual_size))

        def in_code(address):
            return any(lo <= address < hi for lo, hi in code_ranges)

        handles = {}
        for tid in list_threads(k, args.pid):
            if args.tid and tid not in args.tid:
                continue
            h = k.OpenThread(0x0002 | 0x0008 | 0x0040, False, tid)
            if h:
                handles[tid] = h
        start_cpu = {tid: cpu_seconds(k, h) for tid, h in handles.items()}
        leaf = collections.defaultdict(collections.Counter)
        waiter = collections.defaultdict(collections.Counter)
        gstack = collections.defaultdict(collections.Counter)
        ginclusive = collections.defaultdict(collections.Counter)
        gentry = {}
        samples = collections.Counter()
        buffer = C.create_string_buffer(1248 + 16)
        ctx = (C.addressof(buffer) + 15) & ~15
        period = 1.0 / args.hz
        began = time.perf_counter()
        rounds = 0
        while time.perf_counter() - began < args.seconds:
            round_start = time.perf_counter()
            if guest.available and rounds % 50 == 0:
                guest.refresh()
            for tid, h in handles.items():
                C.c_uint32.from_address(ctx + 48).value = 0x00100001  # CONTEXT_AMD64|CONTROL
                if k.SuspendThread(h) == 0xFFFFFFFF:
                    continue
                stack = b""
                captured = None
                try:
                    if not k.GetThreadContext(h, ctx):
                        continue
                    rsp = C.c_uint64.from_address(ctx + 152).value
                    rip = C.c_uint64.from_address(ctx + 248).value
                    want_chain = args.chain_all or not in_main(rip)
                    if want_chain:  # read while suspended so the frames are coherent
                        try:
                            stack = process.read(rsp, args.stack_bytes)
                        except OSError:  # near the stack base: keep the in-page prefix
                            try:
                                stack = process.read(rsp, 4096 - (rsp & 4095))
                            except OSError:
                                stack = b""
                    if guest.available:
                        try:
                            captured = guest.capture(tid)
                        except OSError:
                            captured = None
                finally:
                    k.ResumeThread(h)
                samples[tid] += 1
                name = symbol(rip)
                leaf[tid][name] += 1
                if want_chain:
                    chain = []
                    for (value,) in struct.iter_unpack("<Q", stack[:len(stack) // 8 * 8]):
                        if in_code(value):
                            s = symbol(value)
                            if not chain or chain[-1] != s:
                                chain.append(s)
                            if len(chain) == args.depth:
                                break
                    caller = " <- ".join(chain) or "?"
                    waiter[tid][f"{name.split('+')[0]} <- {caller}"] += 1
                if captured:
                    entry, frames = guest.walk(captured)
                    gentry[tid] = entry
                    gstack[tid][" <- ".join(frames) or "?"] += 1
                    for f in set(frames):
                        ginclusive[tid][f] += 1
            rounds += 1
            delay = period - (time.perf_counter() - round_start)
            if delay > 0:
                time.sleep(delay)
        elapsed = time.perf_counter() - began
        end_cpu = {tid: cpu_seconds(k, h) for tid, h in handles.items()}
        for h in handles.values():
            k.CloseHandle(h)

        threads = []
        for tid in handles:
            if start_cpu[tid] is None or end_cpu[tid] is None:
                continue
            used = end_cpu[tid] - start_cpu[tid]
            n = samples[tid] or 1
            threads.append({
                "tid": tid,
                "guest_entry": f"{gentry[tid]:08X}" if tid in gentry else None,
                "cpu_fraction": round(used / elapsed, 4),
                "samples": samples[tid],
                "leaf": [(s, round(c / n, 4)) for s, c in leaf[tid].most_common(args.top)],
                "outside_main": [(s, round(c / n, 4)) for s, c in waiter[tid].most_common(args.top)],
                "guest_stacks": [(s, round(c / n, 4)) for s, c in gstack[tid].most_common(args.top)],
                "guest_inclusive": [(s, round(c / n, 4)) for s, c in ginclusive[tid].most_common(args.top)],
            })
        threads.sort(key=lambda t: t["cpu_fraction"], reverse=True)
        result = {"pid": args.pid, "elapsed": round(elapsed, 3), "rounds": rounds,
                  "rounds_per_second": round(rounds / elapsed, 1),
                  "guest_registry": guest.available,
                  "total_cpu_cores": round(sum(t["cpu_fraction"] for t in threads), 3),
                  "threads": threads}
        if args.json:
            args.json.write_text(json.dumps(result, indent=1))
        print(f"pid={args.pid} elapsed={result['elapsed']}s rounds={rounds} ({result['rounds_per_second']}/s) "
              f"threads={len(threads)} total_cpu={result['total_cpu_cores']} cores guest_registry={guest.available}")
        for t in threads:
            if t["cpu_fraction"] < args.min_cpu:
                continue
            print(f"\n== tid {t['tid']} guest_entry={t['guest_entry']} cpu={t['cpu_fraction']*100:.1f}% samples={t['samples']}")
            for s, f in t["leaf"][:6]:
                print(f"   {f*100:5.1f}%  {s}")
            if t["outside_main"]:
                print("   -- host chains (leaf <- exe code frames, heuristic):")
                for s, f in t["outside_main"][:4]:
                    print(f"   {f*100:5.1f}%  {s[:220]}")
            if t["guest_stacks"]:
                print("   -- guest stacks (lr <- callers, exact backchain):")
                for s, f in t["guest_stacks"][:6]:
                    print(f"   {f*100:5.1f}%  {s[:220]}")
                print("   -- guest inclusive:")
                print("   " + ", ".join(f"{s} {f*100:.0f}%" for s, f in t["guest_inclusive"][:10]))
        idle = [t["tid"] for t in threads if t["cpu_fraction"] < args.min_cpu]
        print(f"\n{len(idle)} threads below {args.min_cpu*100:.0f}% CPU hidden")
    finally:
        process.close()


if __name__ == "__main__":
    main()
