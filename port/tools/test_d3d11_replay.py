"""Headless hardware replay proof using actual captured shader/resources.

Default --shader-grid uses SYNTHETIC geometry and VS with the captured PS,
constants, blend/alpha state and textures. It is not a captured game draw.
--draw accepts a complete manifest from d3d11_capture.h and runs the real VS
through the unchanged production CPU interpreter before GPU rasterization.
Both paths run a deliberately bounded direct raw-microcode PS translator.
No copyrighted shader bytes/assets are embedded or downloaded by this tool.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
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
int GuestFaultReport(EXCEPTION_POINTERS*) {return EXCEPTION_CONTINUE_SEARCH;}
'''


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise RuntimeError("Production instrumentation anchor changed: " + old[:70])
    return source.replace(old, new, 1)


def compile_replay(folder):
    source = (KERNEL / "gpu_render.cpp").read_text()
    digest = hashlib.sha256(source.encode()).hexdigest()
    source = replace_once(source, '#include "kernel_shared.h"', '#include "replay_adapter.h"')
    source = replace_once(source, "bool GpuExecuteDraw(const uint32_t* regs,",
                          '#include "d3d11_capture.h"\nstatic void ReplayRecordTriangle(const Vertex&,const Vertex&,const Vertex&);\n\nbool GpuExecuteDraw(const uint32_t* regs,')
    source = replace_once(source, "    uint32_t prim = initiator & 0x3F;",
                          "    Ttt2CaptureD3D11Draw(regs,initiator,indexAddr,index32,vsRef,psRef);\n    uint32_t prim = initiator & 0x3F;")
    source = replace_once(source, "        if (!a.valid || !b.valid || !c.valid) return;",
                          "        if (!a.valid || !b.valid || !c.valid) return;\n        ReplayRecordTriangle(a,b,c);")
    (folder / "replay_adapter.h").write_text(ADAPTER)
    # Fault injection lives only in the temporary test build. It triggers after
    # successful GPU Map/copy, precisely before the production commit boundary.
    movie = (KERNEL / "gpu_d3d11_movie.h").read_text()
    movie = replace_once(movie, '        Check(device->GetDeviceRemovedReason(),"Movie device status");',
                         '        if(replayFailAfterReadback)throw DeviceFailure("injected post-readback device failure");\n        Check(device->GetDeviceRemovedReason(),"Movie device status");')
    (folder / "gpu_d3d11_movie.h").write_text("extern bool replayFailAfterReadback;\n" + movie)
    exe = compile_one(folder, "d3d11_replay", source + "\n" + (TOOLS / "d3d11_replay.cpp").read_text(), [folder, KERNEL, TOOLS])
    return exe, digest


def synthetic_grid(capture, width, height):
    regs = list(struct.unpack("<32768I", (capture / "regs.bin").read_bytes()))
    # Deliberately synthetic input: a full-screen quad, varied vertex colour and
    # UV spanning the captured atlas. Production RunShader still executes this
    # VS and exports the vertices used by both CPU and D3D11 rasterization.
    va, pa, vb = 0x01000000, 0x01001000, 0xA1002000
    vs = [1 | (6 << 12) | (0x15 << 16), 2 << 12, 0]
    for dst, offset in ((1, 0), (2, 4), (3, 8)):
        vs += [(dst << 12) | (31 << 20) | (2 << 25), (38 << 16) | 0x688, 12 | (offset << 8)]
    for dst, src in ((62, 1), (0, 2), (1, 3)):
        vs += [(50 << 26) | 0xF8000 | dst, 0, (2 << 24) | (src << 16) | (src << 8) | 0xC0000000]
    vertices = []
    corners = [(0, 0, 0, 0), (width, 0, 1, 0), (0, height, 0, 1), (width, height, 1, 1)]
    for at in (0, 1, 2, 1, 3, 2):
        x, y, u, v = corners[at]
        vertices += [x, y, .25, 1, u, v, 0, 1, .35 + .6*u, .3 + .65*v, .9 - .5*u, .3 + .65*v]
    regs[0x2000] = width
    regs[0x2081], regs[0x2082] = 0, (height << 16) | width
    regs[0x2200] &= ~6  # No captured depth buffer: explicit depth-disabled fixture.
    regs[0x2205] &= ~3
    regs[0x2206] = 0x100  # Synthetic VS emits window coordinates.
    regs[0x4800 + 95 * 2], regs[0x4801 + 95 * 2] = vb | 3, len(vertices)
    segments = [(va, struct.pack(">%dI" % len(vs), *vs)),
                (pa, (capture / "ps.bin").read_bytes()),
                (vb, struct.pack(">%df" % len(vertices), *vertices))]
    if (capture / "resources.json").exists():
        resources = json.loads((capture / "resources.json").read_text())
    else:
        resources = [dict(address=int(t["base"], 16), path=t["path"]) for t in json.loads((capture / "capture.json").read_text())["textures"] if "path" in t]
    for item in resources:
        segments.append((item["address"], (capture / item["path"]).read_bytes()))
    return dict(width=width, height=height, initiator=(6 << 16) | (2 << 6) | 4,
                index_address=0, index32=0, vs_address=va, vs_words=len(vs),
                ps_address=pa, ps_words=len(segments[1][1]) // 4), regs, segments


def captured_draw(manifest):
    data = json.loads(manifest.read_text())
    if data.get("version") != 1:
        raise ValueError("Unsupported capture manifest")
    root = manifest.parent
    regs = struct.unpack("<32768I", (root / data["registers"]).read_bytes())
    segments = [(item["address"], (root / item["path"]).read_bytes()) for item in data["segments"]]
    return data, regs, segments


def bundle(path, draw, regs, segments):
    fields = [0x31315244, 1] + [int(draw[k]) for k in ("width", "height", "initiator", "index_address", "index32", "vs_address", "vs_words", "ps_address", "ps_words")] + [len(segments)]
    with path.open("wb") as f:
        f.write(struct.pack("<12I", *fields))
        f.write(struct.pack("<32768I", *regs))
        for address, data in segments:
            f.write(struct.pack("<2I", address, len(data)))
            f.write(data)


def images(output, width, height):
    try:
        from PIL import Image
    except ImportError:
        return
    for name in ("cpu", "gpu", "difference"):
        raw = output / (name + ".bgra")
        if raw.exists():
            Image.frombytes("RGBA", (width, height), raw.read_bytes(), "raw", "BGRA").save(output / (name + ".png"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--shader-grid", type=Path, help="Captured PS/resources; explicitly synthetic geometry")
    source.add_argument("--draw", type=Path, help="Complete captured draw manifest")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--negative-control", action="store_true")
    parser.add_argument("--check-capture", action="store_true", help="Exercise complete capture helper in this isolated process, then replay its bundle")
    parser.add_argument("--runtime-helper", action="store_true", help="Also test the opt-in cached movie backend and its failure-preservation guards")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.shader_grid:
        draw, regs, segments = synthetic_grid(args.shader_grid, args.width, args.height)
        provenance = {"geometry": "synthetic quad; synthetic VS executed by production CPU oracle",
                      "captured": "PS, textures, constants, blend, alpha state",
                      "overrides": "window-coordinate fixture VS, target size/scissor, depth disabled, no culling",
                      "capture": str(args.shader_grid.resolve())}
    else:
        draw, regs, segments = captured_draw(args.draw)
        provenance = {"geometry": "actual captured draw, VS executed by production CPU oracle", "capture": str(args.draw.resolve())}
    provenance["target_initialization"] = "isolated draw on known opaque #203040 background; no preceding game framebuffer"
    provenance["comparison_bound"] = "all channels <=3/255; larger differences allowed only within 1/128 pixel of CPU triangle edges (D3D11 subpixel snapping); reported separately"
    provenance["scope"] = "CPU-renderer parity proof, not independent proof of complete Xenos sampler semantics or fight support"
    provenance["segment_sha256"] = [{"address": a, "bytes": len(b), "sha256": hashlib.sha256(b).hexdigest()} for a, b in segments]
    with tempfile.TemporaryDirectory(prefix="ttt2-d3d11-replay-") as temporary:
        folder = Path(temporary)
        exe, digest = compile_replay(folder)
        provenance["cpu_renderer_sha256"] = digest
        packed = folder / "capture.bin"
        bundle(packed, draw, regs, segments)
        env = dict(os.environ, TTT2_RENDERLOG="0", TTT2_PRIMLOG="0")
        env.pop("TTT2_TEXDUMP", None)
        env.pop("TTT2_MAXPIX", None)
        env.pop("TTT2_D3D11_CAPTURE", None)
        env.pop("TTT2_D3D11_MOVIE", None)
        if args.check_capture:
            env["TTT2_D3D11_CAPTURE"] = str((args.output / "capture-check").resolve())
        results = {}
        for filtering in ("manual", "native"):
            output = args.output / filtering
            run = subprocess.run([str(exe), str(packed), str(output.resolve()), filtering], text=True, capture_output=True, env=env, timeout=90)
            print(run.stdout + run.stderr, end="", flush=True)
            images(output, draw["width"], draw["height"])
            results[filtering] = run.returncode
        if args.runtime_helper:
            output = args.output / "runtime-helper"
            runtime_env = dict(env, TTT2_D3D11_MOVIE="1")
            run = subprocess.run([str(exe), str(packed), str(output.resolve()), "runtime"], text=True, capture_output=True, env=runtime_env, timeout=90)
            print(run.stdout + run.stderr, end="", flush=True)
            images(output, draw["width"], draw["height"])
            results["runtime-helper"] = run.returncode
        (args.output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
        if args.check_capture:
            captures = sorted((args.output / "capture-check").glob("*/draw.json"))
            if len(captures) != 2:
                raise RuntimeError("Expected one independently named capture per replay process")
            recaptured, recaptured_regs, recaptured_segments = captured_draw(captures[-1])
            check_bundle = folder / "recaptured.bin"
            bundle(check_bundle, recaptured, recaptured_regs, recaptured_segments)
            env.pop("TTT2_D3D11_CAPTURE", None)
            output = args.output / "recaptured"
            run = subprocess.run([str(exe), str(check_bundle), str(output.resolve()), "native"], text=True, capture_output=True, env=env, timeout=90)
            print(run.stdout + run.stderr, end="", flush=True)
            if run.returncode or (output / "cpu.bgra").read_bytes() != (args.output / "native/cpu.bgra").read_bytes() or (output / "gpu.bgra").read_bytes() != (args.output / "native/gpu.bgra").read_bytes():
                raise RuntimeError("Complete capture did not preserve exact replay outputs")
            print("PASS complete-capture roundtrip: exact CPU and GPU images preserved")
            # Capture error paths must leave the game caller running and must
            # never publish a manifest for an incomplete/unsafe snapshot.
            capture_env = dict(env)
            def capture_case(name, case_draw, case_segments, expected, blocked=False):
                dest = folder / ("guard-" + name)
                if blocked:
                    dest.write_text("ordinary file blocks directory creation")
                capture_env["TTT2_D3D11_CAPTURE"] = str(dest)
                case_bundle = folder / (name + ".bin")
                bundle(case_bundle, case_draw, regs, case_segments)
                run = subprocess.run([str(exe), str(case_bundle), str(folder / "unused"), "capture-only"], text=True, capture_output=True, env=capture_env, timeout=30)
                manifests = list(dest.glob("*/draw.json")) if dest.is_dir() else []
                if run.returncode or len(manifests) != expected:
                    raise RuntimeError("Capture guard failed: " + name + run.stdout + run.stderr)
                print("PASS capture guard:", name)
            capture_case("one-per-format", draw, segments, 1)
            capture_case("write-error", draw, segments, 0, blocked=True)
            vertex_address = (regs[0x4800 + 95*2] & ~3) | 0xA0000000
            missing = [(a, b) for a, b in segments if a != vertex_address]
            capture_case("unreadable-vertex", draw, missing, 0)
            bad_draw = dict(draw, index_address=0xA1010000, index32=1, initiator=draw["initiator"] & ~(3 << 6))
            bad_segments = segments + [(bad_draw["index_address"], struct.pack(">6I", 0, 1, 2, 3, 4, 0x200000))]
            capture_case("oversize-index", bad_draw, bad_segments, 0)
        if args.negative_control:
            output = args.output / "negative-color"
            run = subprocess.run([str(exe), str(packed), str(output.resolve()), "native", "negative-color"], text=True, capture_output=True, env=env, timeout=90)
            print(run.stdout + run.stderr, end="", flush=True)
            if run.returncode != 1 or not (output / "result.json").exists() or json.loads((output / "result.json").read_text())["out_of_bounds_pixels"] == 0:
                raise RuntimeError("Wrong translated color negative control was not detected")
            print("PASS translated-color negative control rejected")
            # Mutate fields of the captured instructions, never substitute an
            # expected image or silently ignore unsupported shader behavior.
            for name, word, mask, value, message in (
                ("loop", 1, 0xF000, 7 << 12, "Conditional/loop/jump"),
                ("volume-fetch", 8, 0xC000, 2 << 14, "plain unpredicated 2D"),
                ("unsupported-scalar", 9, 0xFC000000, 40 << 26, "Scalar ALU opcode"),
            ):
                shader = next(b for a, b in segments if a == draw["ps_address"])
                words = list(struct.unpack(">%dI" % (len(shader)//4), shader))
                if len(words) != 15:
                    continue  # These mutations exercise the requested 15-word UI family.
                words[word] = (words[word] & ~mask) | value
                bad = struct.pack(">%dI" % len(words), *words)
                malformed = [(a, bad if a == draw["ps_address"] else b) for a, b in segments]
                bad_bundle = folder / (name + ".bin")
                bundle(bad_bundle, draw, regs, malformed)
                run = subprocess.run([str(exe), str(bad_bundle), str(folder / name), "native"], text=True, capture_output=True, env=env, timeout=30)
                if run.returncode != 2 or message not in run.stderr:
                    raise RuntimeError("Unsupported shader escaped fail-closed translation: " + name + run.stdout + run.stderr)
                print("PASS unsupported shader rejected:", name)
        if any(results.values()):
            raise SystemExit("Pixel comparison failed: " + str(results))


if __name__ == "__main__":
    main()
