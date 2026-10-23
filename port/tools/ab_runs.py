"""Alternate unattended fight runs between settings and compare frame pacing (needs the game
assets; logs under re/). Variants interleave (A B A B ...) so drift in machine state hits both.

    python port/tools/ab_runs.py --cmd run_vs_tagtulip.cmd --variant on= --variant off=TTT2_EVENT_HANDOFF=0 --repeat 2

Each run: launch re/<cmd> with the variant's environment, wait for --frames presents, stop the
game, average the [frame] swap 10-s windows after --skip windows (TTT2_FRAMETIME=1 must be set
by the .cmd, as make_vs_script.py does).
"""
import argparse
import os
import re
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def stop_game():
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Get-Process tekken_recomp -ErrorAction SilentlyContinue | Stop-Process -Confirm:$false"],
                   capture_output=True, timeout=120)
    time.sleep(1.5)


def latest_frame(log):
    found = re.findall(r"frame=(\d+)", log.read_text(errors="replace")) if log.exists() else []
    return int(found[-1]) if found else 0


def windows(log, skip):
    return [(int(a), int(b)) for a, b in re.findall(r"^\[frame\] swap 10s: (\d+) frames, >17\.5ms \d+, >25ms (\d+)",
                                                    log.read_text(errors="replace"), re.M)][skip:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmd", required=True)
    ap.add_argument("--variant", action="append", required=True, help="name=VAR=VALUE[,VAR=VALUE] (empty: defaults)")
    ap.add_argument("--repeat", type=int, default=2)
    ap.add_argument("--frames", type=int, default=13500, help="stop after this many presents")
    ap.add_argument("--skip", type=int, default=12, help="10-s windows before the fight")
    args = ap.parse_args()
    variants = []
    for v in args.variant:
        name, _, rest = v.partition("=")
        env = {}
        for pair in filter(None, rest.split(",")):
            k, _, val = pair.partition("=")
            env[k] = val
        variants.append((name, env))
    results = {name: [] for name, _ in variants}
    for rep in range(args.repeat):
        for name, env in variants:
            log = ROOT / "re" / f"ab_{Path(args.cmd).stem}_{name}_{rep}.txt"
            stop_game()
            subprocess.Popen(["cmd.exe", "/d", "/c", str(ROOT / "re" / args.cmd), log.name], cwd=str(ROOT),
                             env=dict(os.environ, **env), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             creationflags=0x08000000)
            start = time.time()
            while latest_frame(log) < args.frames and time.time() - start < 900:
                time.sleep(3)
            stop_game()
            w = windows(log, args.skip)
            frames = [a for a, _ in w]
            avg = sum(frames) / len(frames) if frames else 0
            results[name].append(avg)
            print(f"{name} #{rep}: {len(w)} windows avg {avg:.1f}/10s, >25ms {sum(b for _, b in w)} | {' '.join(map(str, frames))}",
                  flush=True)
    for name, vals in results.items():
        print(f"== {name}: runs {' '.join('%.1f' % v for v in vals)} mean {sum(vals) / len(vals):.1f}")


if __name__ == "__main__":
    main()
