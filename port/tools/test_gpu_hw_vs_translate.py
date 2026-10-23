"""Parity of the hardware vertex-shader translator against the CPU interpreter.

Compiles the production gpu_render.cpp (with the hardware backend headers) plus
gpu_hw_vs_parity.cpp, then runs every captured vertex shader in port/shaders/vs_*.bin
(local game captures - never distributed) for 512 vertex indices over randomized vertex
data, constants and viewport state through both the interpreter (+ the shadeVertex window
transform) and the translated HLSL on the GPU. The negative control breaks the 8_8_8_8
vertex-fetch component order and must be detected.

  python port/tools/test_gpu_hw_vs_translate.py [--negative-control] [shader.bin ...]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_gpu_hw_translate import ADAPTER, BACKEND_HEADERS, KERNEL, TOOLS, ROOT
from test_vcmpgtuh import compile_one


def build(folder, negative):
    source = (KERNEL / "gpu_render.cpp").read_text()
    if source.count('#include "kernel_shared.h"') != 1:
        raise SystemExit("FAIL: adapter anchor changed")
    source = source.replace('#include "kernel_shared.h"', '#include "parity_adapter.h"', 1)
    source = "#define TTT2_GPU_HW_BACKEND 1\n#include <map>\n" + source
    (folder / "parity_adapter.h").write_text(ADAPTER)
    translate = (KERNEL / "gpu_hw_translate.h").read_text()
    if negative:
        old = 'v = "float4(XN8(d0, " + S + "), XN8(d0 >> 8, " + S + "), XN8(d0 >> 16, " + S + "), XN8(d0 >> 24, " + S + "))";'
        if translate.count(old) != 1:
            raise SystemExit("FAIL: negative-control anchor changed")
        translate = translate.replace(old, 'v = "float4(XN8(d0 >> 24, " + S + "), XN8(d0 >> 16, " + S + "), XN8(d0 >> 8, " + S + "), XN8(d0, " + S + "))";')
    (folder / "gpu_hw_translate.h").write_text(translate)
    for name in BACKEND_HEADERS:
        (folder / name).write_text((KERNEL / name).read_text())
    return compile_one(folder, "vsparity", source + "\n" + (TOOLS / "gpu_hw_vs_parity.cpp").read_text(),
                       [folder, TOOLS])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--negative-control", action="store_true")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    parser.add_argument("shaders", nargs="*", type=Path)
    args = parser.parse_args()
    shaders = args.shaders or sorted((ROOT / "port/shaders").glob("vs_*.bin"))
    if not shaders:
        raise SystemExit("FAIL: no vertex shaders found in port/shaders")
    with tempfile.TemporaryDirectory(prefix="ttt2-hw-vs-") as temp:
        exe = build(Path(temp), args.negative_control)
        jobs = max(1, min(args.jobs, len(shaders)))
        chunks = [shaders[i::jobs] for i in range(jobs)]
        procs = []
        for i, chunk in enumerate(chunks):
            out_file = open(Path(temp) / f"out{i}.txt", "w+")
            err_file = open(Path(temp) / f"err{i}.txt", "w+")
            procs.append((subprocess.Popen([str(exe)] + [str(s) for s in chunk], stdout=out_file,
                                           stderr=err_file, text=True), out_file, err_file))
        failed, stdout, stderr, skipped = False, [], [], {}
        totals = {}
        for proc, out_file, err_file in procs:
            proc.wait(timeout=1800)
            out_file.seek(0); err_file.seek(0)
            out, err = out_file.read(), err_file.read()
            out_file.close(); err_file.close()
            failed |= proc.returncode != 0
            for line in out.splitlines():
                if line.startswith("SUMMARY"):
                    for field in line.split()[1:]:
                        key, value = field.split("=")
                        totals[key] = totals.get(key, 0) + int(value)
                elif line.startswith("SKIPPED x"):
                    count, reason = line[len("SKIPPED x"):].split(": ", 1)
                    skipped[reason] = skipped.get(reason, 0) + int(count)
                else:
                    stdout.append(line)
            if err.strip():
                stderr.append(err.strip()[-2000:])
        print("\n".join(sorted(stdout, key=lambda l: l.split(" ", 2)[1] if " " in l else l)))
        for reason, count in sorted(skipped.items(), key=lambda kv: -kv[1]):
            print(f"SKIPPED x{count}: {reason}")
        print("SUMMARY " + " ".join(f"{k}={v}" for k, v in totals.items()))
        if stderr:
            print("\n".join(stderr)[-4000:])
        if args.negative_control:
            if not failed:
                raise SystemExit("FAIL: negative control (8_8_8_8 order) not detected")
            print("PASS negative control detected")
            return
        if failed:
            raise SystemExit("FAIL: vertex translator/interpreter parity")
        print(f"PASS {len(shaders)} captured vertex shaders: translator matches the interpreter")


if __name__ == "__main__":
    main()
