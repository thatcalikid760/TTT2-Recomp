"""Parity of the hardware pixel-shader translator against the CPU interpreter.

Compiles the production gpu_render.cpp (with the hardware backend headers) plus
gpu_hw_translate_parity.cpp, then runs every captured pixel shader in
port/shaders/ps_*.bin (local game captures - never distributed) on 512 random
input sets through both the interpreter and the translated HLSL on the GPU.
The negative control breaks one translated opcode and must be detected.

  python port/tools/test_gpu_hw_translate.py [--negative-control] [shader.bin ...]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
KERNEL = ROOT / "port/src/runtime/kernel"
TOOLS = ROOT / "port/tools"
ADAPTER = r'''
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>
uint8_t* g_base;
uint8_t GRead8(uint8_t* b,uint32_t a){return b[a];}
uint16_t GRead16(uint8_t* b,uint32_t a){uint16_t v;std::memcpy(&v,b+a,2);return __builtin_bswap16(v);}
uint32_t GRead32(uint8_t* b,uint32_t a){uint32_t v;std::memcpy(&v,b+a,4);return __builtin_bswap32(v);}
inline void GWrite32(uint8_t* b,uint32_t a,uint32_t v){v=__builtin_bswap32(v);std::memcpy(b+a,&v,4);}
int GuestFaultReport(EXCEPTION_POINTERS*) {return EXCEPTION_CONTINUE_SEARCH;}
bool DiagSkinDescribe(uint32_t,char*,size_t) {return false;}
#pragma comment(lib, "user32.lib")   // the presenter's window queries (GetClientRect ...)
#include "display_settings.h"
namespace display_settings {   // the backend's display settings: windowed 1280x720 (render scale 1)
const std::vector<Resolution>& Resolutions() { static const std::vector<Resolution> r{ { 1280, 720 } }; return r; }
int GetMode() { return Windowed; }
int GetResolutionIndex() { return 0; }
Resolution GetResolution() { return { 1280, 720 }; }
void SetMode(int) {}
void SetResolutionIndex(int) {}
uint32_t Generation() { return 0; }
}
'''
# Kernel headers the hardware backend includes, copied next to the test translation unit.
BACKEND_HEADERS = ("gpu_hw_d3d11.h", "gpu_render.h", "gpu_draw_profile.h", "gpu_d3d11_movie.h", "gpu_shader_subset.h",
                   "display_settings.h")


def build(folder, negative):
    source = (KERNEL / "gpu_render.cpp").read_text()
    if source.count('#include "kernel_shared.h"') != 1:
        raise SystemExit("FAIL: adapter anchor changed")
    source = source.replace('#include "kernel_shared.h"', '#include "parity_adapter.h"', 1)
    source = "#define TTT2_GPU_HW_BACKEND 1\n" + source
    (folder / "parity_adapter.h").write_text(ADAPTER)
    translate = (KERNEL / "gpu_hw_translate.h").read_text()
    if negative:
        old = 'case V_MAD:   res = "va * vb + vc"; break;'
        if translate.count(old) != 1:
            raise SystemExit("FAIL: negative-control anchor changed")
        translate = translate.replace(old, 'case V_MAD:   res = "va * vb - vc"; break;')
    (folder / "gpu_hw_translate.h").write_text(translate)
    for name in BACKEND_HEADERS:
        (folder / name).write_text((KERNEL / name).read_text())
    return compile_one(folder, "parity", source + "\n" + (TOOLS / "gpu_hw_translate_parity.cpp").read_text(),
                       [folder, TOOLS])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--negative-control", action="store_true")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    parser.add_argument("shaders", nargs="*", type=Path)
    args = parser.parse_args()
    shaders = args.shaders or sorted((ROOT / "port/shaders").glob("ps_*.bin"))
    if not shaders:
        raise SystemExit("FAIL: no pixel shaders found in port/shaders")
    with tempfile.TemporaryDirectory(prefix="ttt2-hw-translate-") as temp:
        exe = build(Path(temp), args.negative_control)
        # FXC compiles dominate (three per shader): run chunks in parallel processes. Each
        # shader's inputs are seeded from its file name, so chunking does not change results.
        jobs = max(1, min(args.jobs, len(shaders)))
        chunks = [shaders[i::jobs] for i in range(jobs)]
        # Output goes to files: draining pipes one process at a time would stall the others.
        procs = []
        for i, chunk in enumerate(chunks):
            out_file = open(Path(temp) / f"out{i}.txt", "w+")
            err_file = open(Path(temp) / f"err{i}.txt", "w+")
            procs.append((subprocess.Popen([str(exe)] + [str(s) for s in chunk], stdout=out_file,
                                           stderr=err_file, text=True), out_file, err_file))
        failed, stdout, stderr = False, [], []
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
                else:
                    stdout.append(line)
            if err.strip():
                stderr.append(err.strip()[-2000:])
        print("\n".join(sorted(stdout, key=lambda l: l.split(" ", 2)[1] if " " in l else l)))
        print("SUMMARY " + " ".join(f"{k}={v}" for k, v in totals.items()))
        if stderr:
            print("\n".join(stderr)[-4000:])
        if args.negative_control:
            if not failed:
                raise SystemExit("FAIL: negative control (MAD sign flip) not detected")
            print("PASS negative control detected")
            return
        if failed:
            raise SystemExit("FAIL: translator/interpreter parity")
        print(f"PASS {len(shaders)} captured pixel shaders: translator matches the interpreter")


if __name__ == "__main__":
    main()
