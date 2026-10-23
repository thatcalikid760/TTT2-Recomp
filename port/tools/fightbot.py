"""Play a running tekken_recomp: both players fight with random Tekken inputs, through rounds,
KOs, results and the next character select, so timing runs see real gameplay instead of an idle
fight. Presses go through C:/ttt2/port/pad_inject.txt; "&" lines press together with the line
before (both players at once). Never START, BACK, B or Y (see MOVES).

    python port/tools/fightbot.py --seconds 300 --shots 30 --shot-dir out/   # screenshot every 30 s
    ... --join-ref join.png       # START for both players whenever the after-match join screen shows
"""
import argparse
import os
import random
import time
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

INJECT = Path("C:/ttt2/port/pad_inject.txt")
FRAME = Path("C:/ttt2/port/fbdump.bmp")

# Moves in the player's own frame: F = towards the opponent, K = away. X = punch 1, A = kick 3,
# LB = tag. Only buttons that are harmless at character select (X costume, A confirm, LB solo /
# tag): Y opens PRESET TAG there and B returns to the main menu, so a bot using them gets stuck.
MOVES = [
    ["X"], ["A"], ["X", "X"], ["X", "A"], ["X", "X", "A"], ["X+A"], ["DOWN+A"], ["DOWN+F+X"],
    ["F", "F+X"], ["F", "F+A"], ["F+X"], ["DOWN", "DOWN+F", "F+X"], ["F", "DOWN", "DOWN+F", "F+X"],
    ["DOWN", "DOWN+K", "K+A"], ["K", "K"], ["F", "F"], ["UP"], ["DOWN"], ["UP+F+A"], ["DOWN+K"],
    ["F+A", "X"], ["LB"], ["DOWN+X"], ["UP+A"], ["K+X"], ["F", "F", "F", "X"],
]
HOLDS = {"K": 350, "F": 90}


def press(player, names, forward):
    back = "LEFT" if forward == "RIGHT" else "RIGHT"
    parts = [forward if n == "F" else back if n == "K" else n for n in names.split("+")]
    return ("P2+" if player else "") + "+".join(parts)


def batch(rng, p1_only=False):
    """About a second of input: each step presses one move element for P1 and one for P2."""
    p1, p2 = rng.choice(MOVES), ([] if p1_only else rng.choice(MOVES))
    lines = []
    for i in range(max(len(p1), len(p2))):
        hold = rng.choice((60, 80, 100))
        gap = rng.choice((40, 60, 120))
        first = True
        for player, move, forward in ((0, p1, "RIGHT"), (1, p2, "LEFT")):
            if i >= len(move):
                continue
            h = HOLDS.get(move[i], hold)
            lines.append(("&" if not first else "") + f"{press(player, move[i], forward)} {h} {gap}\n")
            first = False
    return lines


def shot(out):
    before = FRAME.stat().st_mtime if FRAME.exists() else 0
    FRAME.with_suffix(".request").touch()
    deadline = time.time() + 3
    while time.time() < deadline:
        if FRAME.exists() and FRAME.stat().st_mtime > before:
            time.sleep(0.3)
            try:
                Image.open(FRAME).save(out)
                return True
            except OSError:
                pass
        time.sleep(0.1)
    return False


def looks_like(image_path, reference):
    """Mean grey-level difference of 64x36 thumbnails (the join screen: ~0.5; other screens 12+)."""
    def thumb(f):
        return Image.open(f).convert("L").resize((64, 36))
    try:
        return ImageStat.Stat(ImageChops.difference(thumb(reference), thumb(image_path))).mean[0] < 4
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=300)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--p1-only", action="store_true", help="player 1 only (Arcade: a second pad would be a new challenger)")
    ap.add_argument("--shots", type=float, default=0, help="screenshot every N seconds (0: none)")
    ap.add_argument("--shot-dir", default=".")
    ap.add_argument("--join-ref", help="screenshot of 'Press START button of controller in use' (after a match): "
                                       "when a screenshot matches it, both players press START")
    args = ap.parse_args()
    rng = random.Random(args.seed)
    end = time.time() + args.seconds
    next_shot = time.time() + args.shots if args.shots else float("inf")
    n = 0
    queued_until = 0.0
    while time.time() < end:
        if INJECT.exists():         # the game takes the file within ~100 ms
            time.sleep(0.02)
            continue
        lines = []
        for _ in range(rng.choice((1, 2))):
            lines += batch(rng, args.p1_only)
        tmp = INJECT.with_suffix(".tmp")
        tmp.write_text("".join(lines))
        os.replace(tmp, INJECT)
        n += 1
        # The game runs queued presses back to back from when it takes the file (or when the
        # previous batch ends): keep the same schedule and write the next batch ~0.15 s before
        # this one runs out, so presses neither stall nor pile up ahead.
        at = previous = 0
        for line in lines:
            name, hold, gap = line.split()
            start = previous if name.startswith("&") else at
            at = max(at, start + int(hold) + int(gap))
            previous = start
        queued_until = max(time.time(), queued_until) + at / 1000
        time.sleep(max(0.02, queued_until - time.time() - 0.15))
        if time.time() >= next_shot:
            name = Path(args.shot_dir) / f"play_{int(args.seconds - (end - time.time())):04d}.png"
            if shot(name) and args.join_ref and looks_like(name, args.join_ref):
                while INJECT.exists():
                    time.sleep(0.02)
                INJECT.write_text("START 150 600\nP2+START 150 600\n")
                queued_until = time.time() + 1.5
                print(f"join screen at {name.name}: START for both players")
            next_shot += args.shots
    print(f"{n} batches")


if __name__ == "__main__":
    main()
