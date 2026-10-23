"""Drive a running tekken_recomp by injected pad presses and grab what is on screen.

Each argument is one press, as in C:/ttt2/port/pad_inject.txt: BUTTONS[:hold_ms[:gap_ms]], with
BUTTONS = names joined by '+' (START BACK UP DOWN LEFT RIGHT A B X Y LB RB LS RS) and P2 for player 2.
After the presses (and --wait seconds) the latest presented frame (fbdump.bmp, written every 60
presents) is saved as PNG for viewing.

    python port/tools/nav.py DOWN A --wait 2 --out shot.png
    python port/tools/nav.py P2+START --out p2.png
    python port/tools/nav.py --out now.png             # just a screenshot
    python port/tools/nav.py A --instance B --out b.png  # the instance started with TTT2_INSTANCE=B
"""
import argparse
import os
import time
from pathlib import Path

from PIL import Image

INJECT = Path("C:/ttt2/port/pad_inject.txt")
FRAME = Path("C:/ttt2/port/fbdump.bmp")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("presses", nargs="*")
    ap.add_argument("--wait", type=float, default=1.5, help="seconds after the last press")
    ap.add_argument("--out", required=True)
    ap.add_argument("--instance", default="", help="TTT2_INSTANCE tag of the game to drive (two-instance tests)")
    args = ap.parse_args()
    inject = Path(f"C:/ttt2/port/pad_inject{args.instance}.txt")
    frame = Path(f"C:/ttt2/port/fbdump{args.instance}.bmp")
    total_ms = 0
    lines = []
    for press in args.presses:
        parts = press.split(":")
        hold = int(parts[1]) if len(parts) > 1 else 120
        gap = int(parts[2]) if len(parts) > 2 else 350
        lines.append(f"{parts[0]} {hold} {gap}\n")
        total_ms += hold + gap
    if lines:
        while inject.exists():          # the game consumes the file within ~100 ms
            time.sleep(0.05)
        tmp = inject.with_suffix(".tmp")
        tmp.write_text("".join(lines))
        os.replace(tmp, inject)
    before = frame.stat().st_mtime if frame.exists() else 0
    time.sleep(total_ms / 1000 + args.wait)
    before = max(before, frame.stat().st_mtime if frame.exists() else 0)
    frame.with_suffix(".request").touch()   # games run with TTT2_FBDUMP=request dump on demand
    deadline = time.time() + 5
    while frame.exists() and frame.stat().st_mtime <= before and time.time() < deadline:
        time.sleep(0.1)
    for _ in range(5):
        try:
            Image.open(frame).save(args.out)
            break
        except OSError:                 # caught mid-write: retry
            time.sleep(0.2)
    print(args.out)


if __name__ == "__main__":
    main()
