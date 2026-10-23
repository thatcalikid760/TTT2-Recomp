"""Run unattended two-player VS Battle fights over several stages and character pairs and
report frame pacing per fight (needs the game assets; writes logs under re/, PNGs to --shots).

Each config is (name, stage moves, P1 moves, P2 moves) for port/tools/make_vs_script.py. A run
boots the game, plays the scripted fight for --fight-seconds, takes two screenshots during the
fight (to identify stage and fighters), stops the game and summarises the [frame] swap windows
recorded during the fight (TTT2_FRAMETIME=1).

    python port/tools/sweep_vs.py --shots <dir> [--build build-o1] [--only name,...]
"""
import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
PY = sys.executable
FRAME = Path("C:/ttt2/port/fbdump.bmp")

CONFIGS = [
    ("s1", "LEFT", "", ""),
    ("s2", "LEFT,LEFT,LEFT", "RIGHT,RIGHT", "LEFT,LEFT"),
    ("s3", "LEFT,LEFT,LEFT,LEFT,LEFT,DOWN", "DOWN", "DOWN"),
    ("s4", "LEFT,LEFT,DOWN,DOWN", "UP", "UP"),
    ("s5", "LEFT,LEFT,LEFT,LEFT,LEFT,LEFT,LEFT,DOWN", "LEFT,LEFT,LEFT", "RIGHT,RIGHT,RIGHT"),
    ("s6", "LEFT,LEFT,LEFT,LEFT,DOWN,DOWN", "DOWN,DOWN", "DOWN,DOWN,RIGHT"),
    ("s7", "LEFT,LEFT,LEFT,LEFT,LEFT,LEFT,LEFT,LEFT,LEFT", "RIGHT,RIGHT,RIGHT,RIGHT", "UP,LEFT"),
]


def ps(command):
    return subprocess.run(["powershell", "-NoProfile", "-Command", command], capture_output=True, text=True, timeout=120)


def stop_game():
    ps("Get-Process tekken_recomp -ErrorAction SilentlyContinue | Stop-Process -Confirm:$false")
    time.sleep(1.5)


def latest_frame(log):
    found = re.findall(r"frame=(\d+)", log.read_text(errors="replace")) if log.exists() else []
    return int(found[-1]) if found else 0


def shot(out):
    for _ in range(10):
        try:
            Image.open(FRAME).save(out)
            return
        except OSError:
            time.sleep(0.3)


def summarise(log, skip_windows):
    windows = re.findall(r"^\[frame\] swap 10s: (\d+) frames, >17\.5ms (\d+), >25ms (\d+), >33ms (\d+), >50ms (\d+), longest ([\d.]+) ms",
                         log.read_text(errors="replace"), re.M)[skip_windows:]
    if not windows:
        return "no fight windows"
    frames = [int(w[0]) for w in windows]
    return ("%d windows avg %.1f/10s min %d, >25ms %d, >33ms %d, >50ms %d, longest %.0f ms | %s" % (
        len(windows), sum(frames) / len(frames), min(frames), sum(int(w[2]) for w in windows),
        sum(int(w[3]) for w in windows), sum(int(w[4]) for w in windows), max(float(w[5]) for w in windows),
        " ".join(map(str, frames))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shots", required=True)
    ap.add_argument("--build", default="build-o1")
    ap.add_argument("--fight-seconds", type=int, default=100)
    ap.add_argument("--only", default="")
    ap.add_argument("--tag", default="sweep")
    args = ap.parse_args()
    shots = Path(args.shots)
    shots.mkdir(parents=True, exist_ok=True)
    only = set(filter(None, args.only.split(",")))
    for name, stage, p1, p2 in CONFIGS:
        if only and name not in only:
            continue
        cmd = f"run_{args.tag}_{name}.cmd"
        subprocess.run([PY, str(ROOT / "port/tools/make_vs_script.py"), "--stage", stage, "--p1", p1, "--p2", p2,
                        "--fight-seconds", str(args.fight_seconds), "--out", cmd], check=True, capture_output=True)
        log = ROOT / "re" / f"run_{args.tag}_{name}.txt"
        stop_game()
        env = dict(os.environ, TTT2_BUILD=args.build)
        subprocess.Popen(["cmd.exe", "/d", "/c", str(ROOT / "re" / cmd), log.name], cwd=str(ROOT), env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=0x08000000)
        start = time.time()
        # The fight starts ~115 s after launch (frame ~6900); screenshots 25 s and 70 s into it.
        while latest_frame(log) < 8400 and time.time() - start < 400:
            time.sleep(3)
        shot(shots / f"{args.tag}_{name}_a.png")
        while latest_frame(log) < 11100 and time.time() - start < 500:
            time.sleep(3)
        shot(shots / f"{args.tag}_{name}_b.png")
        end_frame = 6900 + args.fight_seconds * 60
        while latest_frame(log) < end_frame and time.time() - start < 600:
            time.sleep(3)
        stop_game()
        # 10-s windows before the fight (boot, menus, loading): ~12.
        print(f"{name} stage={stage or '-'} p1={p1 or '-'} p2={p2 or '-'}: {summarise(log, 12)}", flush=True)


if __name__ == "__main__":
    main()
