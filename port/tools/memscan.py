"""Cheat-Engine-style value scan of the running port's guest memory (diagnostics).

The guest's 4 GB address space is one reservation in tekken_recomp.exe; only pages the game has
touched are resident, so the scan reads just those (QueryWorkingSetEx) and compares aligned
big-endian 32-bit words. Candidates (guest addresses) persist in a state file between calls:

  python port/tools/memscan.py new 14        # every word == 14
  python port/tools/memscan.py next 11       # keep the candidates that now hold 11
  python port/tools/memscan.py list          # candidates and their current values
"""
import ctypes
import ctypes.wintypes as wt
import json
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

STATE = Path(__file__).resolve().parent.parent / "memscan_state.json"
GUEST_SPAN = 0x100000000
PAGE = 0x1000

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p), ("AllocationProtect", wt.DWORD),
                ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD), ("Protect", wt.DWORD),
                ("Type", wt.DWORD)]


class WSEX(ctypes.Structure):
    _fields_ = [("VirtualAddress", ctypes.c_void_p), ("VirtualAttributes", ctypes.c_size_t)]


def game_pid():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq tekken_recomp.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout.strip()
    if not out.startswith('"'):
        sys.exit("tekken_recomp.exe is not running")
    return int(out.split('","')[1])


def open_game():
    h = k32.OpenProcess(0x0400 | 0x0010, False, game_pid())   # QUERY_INFORMATION | VM_READ
    if not h:
        sys.exit(f"OpenProcess failed ({ctypes.get_last_error()})")
    return h


def guest_base(h):
    """The allocation base of the 4 GB+ reservation."""
    addr, mbi = 0, MBI()
    alloc, total = None, 0
    while addr < 1 << 47 and k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        base = mbi.AllocationBase or 0
        if base != alloc:
            alloc, total = base, 0
        total += mbi.RegionSize
        if alloc and total >= GUEST_SPAN:
            return alloc
        addr = (mbi.BaseAddress or 0) + (mbi.RegionSize or PAGE)
    sys.exit("guest reservation not found")


def resident_runs(h, base, lo=0, hi=GUEST_SPAN):
    """Guest [start, end) runs whose pages are resident in the game's working set."""
    runs, chunk = [], 1 << 16
    for first in range(lo, hi, chunk * PAGE):
        n = min(chunk, (hi - first) // PAGE)
        arr = (WSEX * n)()
        for i in range(n):
            arr[i].VirtualAddress = base + first + i * PAGE
        if not psapi.QueryWorkingSetEx(h, arr, ctypes.sizeof(arr)):
            continue
        start = None
        for i in range(n):
            valid = arr[i].VirtualAttributes & 1
            g = first + i * PAGE
            if valid and start is None:
                start = g
            elif not valid and start is not None:
                runs.append((start, g)); start = None
        if start is not None:
            runs.append((start, first + n * PAGE))
    merged = []
    for s, e in runs:
        if merged and merged[-1][1] == s:
            merged[-1] = (merged[-1][0], e)
        else:
            merged.append((s, e))
    return merged


def read(h, base, guest, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(base + guest), buf, size, ctypes.byref(got)):
        return None
    return buf.raw[:got.value]


def scan_new(h, base, value):
    found = []
    for s, e in resident_runs(h, base):
        for off in range(s, e, 1 << 24):
            size = min(1 << 24, e - off)
            data = read(h, base, off, size)
            if not data:
                continue
            words = np.frombuffer(data[: len(data) // 4 * 4], dtype=">u4")
            idx = np.nonzero(words == value)[0]
            found.extend(int(off + i * 4) for i in idx)
    return found


def current(h, base, addrs):
    vals = {}
    for a in addrs:
        d = read(h, base, a, 4)
        vals[a] = struct.unpack(">I", d)[0] if d and len(d) == 4 else None
    return vals


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    h = open_game()
    base = guest_base(h)
    if cmd == "write":                     # write <guest addr> <value>: one big-endian word
        hw = k32.OpenProcess(0x0400 | 0x0010 | 0x0020 | 0x0008, False, game_pid())   # + VM_WRITE | VM_OPERATION
        a, v = int(sys.argv[2], 16), int(sys.argv[3], 0)
        data = struct.pack(">I", v)
        ok = k32.WriteProcessMemory(hw, ctypes.c_void_p(base + a), data, 4, None)
        print(f"{a:08X} <- {v}: {'ok' if ok else 'failed'}")
        return
    if cmd == "new":
        cands = scan_new(h, base, int(sys.argv[2], 0))
    elif cmd == "next":
        prev = json.loads(STATE.read_text())["cands"]
        want = int(sys.argv[2], 0)
        cands = [a for a, v in current(h, base, prev).items() if v == want]
    else:
        cands = json.loads(STATE.read_text())["cands"]
    STATE.write_text(json.dumps({"cands": cands}))
    print(f"guest base {base:#x}: {len(cands)} candidates")
    if len(cands) <= 40:
        for a, v in current(h, base, cands).items():
            print(f"  {a:08X} = {v}")


if __name__ == "__main__":
    main()
