"""XMA context emulation regression (port/src/runtime/kernel/xma_core.h); no full port build.

Builds port/third_party/ffmpeg_xma/test (the port's minimal FFmpeg + xma_core_test), takes XMA2
sounds from the local game archives (re/game_files_x, never shipped), decodes them with an
independent full ffmpeg (PATH or %FFMPEG%) as the reference, and drives the context emulation
the way TTT2 does: double-buffered input two packets at a time, a 6-block output ring consumed
after every kick, then the same sound again on the reset context. Needs one 512-sample frame
per frame in the stream (what NUSound counts to end a one-shot) and bit-exact PCM (<= 1 LSB)
after the first frame and the 64-sample encoder delay, which the reference drops. Negative
control: a title that never re-validates a full ring must fail.

Samples: the first mono and the first long stereo XMA2 RIFF found in the archives.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ARCHIVES = ROOT / "re/game_files_x"
TEST_SRC = Path(r"C:\ttt2\port\third_party\ffmpeg_xma\test")   # space-free junction (FFmpeg)
BUILD = Path(r"C:\ttt2\port\build-xmatest")
VS = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools")
CMAKE = VS / r"Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
NINJA = VS / r"Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"


def find_samples():
    """(archive, offset, size) of the first mono XMA2 RIFF and the first stereo one > 400 KB."""
    want = {1: None, 2: None}
    for archive in sorted(ARCHIVES.glob("data*.bin")):
        with open(archive, "rb") as f:
            offset, tail = 0, b""
            while not all(want.values()):
                chunk = f.read(64 << 20)
                if not chunk:
                    break
                buf, base = tail + chunk, offset - len(tail)
                i = buf.find(b"RIFF")
                while i != -1 and i + 40 <= len(buf):
                    if buf[i + 8:i + 16] == b"WAVEfmt ":
                        tag, channels, rate = struct.unpack("<HHI", buf[i + 20:i + 28])
                        size = struct.unpack("<I", buf[i + 4:i + 8])[0] + 8
                        if tag == 0x166 and channels in want and not want[channels] and rate in (24000, 32000, 44100, 48000):
                            if channels == 1 or size > 400000:
                                want[channels] = (archive, base + i, size)
                    i = buf.find(b"RIFF", i + 4)
                tail, offset = buf[-64:], offset + len(chunk)
        if all(want.values()):
            break
    return [v for v in want.values() if v]


def build():
    command = (f'@echo off\ncall "{VS}\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
               f'set "PATH=C:\\Program Files\\LLVM\\bin;%PATH%"\n'
               f'"{CMAKE}" -S "{TEST_SRC}" -B "{BUILD}" -G Ninja -DCMAKE_MAKE_PROGRAM="{NINJA}" '
               f'-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release >nul || exit /b 1\n'
               f'"{CMAKE}" --build "{BUILD}" --target xma_core_test --parallel 8\n')
    with tempfile.TemporaryDirectory(prefix="ttt2-xmabuild-") as temp:
        batch = Path(temp) / "build.cmd"
        batch.write_text(command, encoding="utf-8")
        result = subprocess.run(["cmd.exe", "/d", "/c", str(batch)], text=True, capture_output=True)
    if result.returncode:
        raise SystemExit("build failed:\n" + result.stdout[-3000:] + result.stderr[-3000:])
    return BUILD / "xma_core_test.exe"


def main():
    ffmpeg = os.environ.get("FFMPEG") or shutil.which("ffmpeg")
    if not ffmpeg:
        raise SystemExit("SKIP: no reference ffmpeg (set FFMPEG or put ffmpeg on PATH)")
    samples = find_samples()
    if len(samples) < 2:
        raise SystemExit("FAIL: no mono and stereo XMA2 samples in re/game_files_x")
    exe = build()
    failed = False
    with tempfile.TemporaryDirectory(prefix="ttt2-xma-") as temp:
        for n, (archive, offset, size) in enumerate(samples):
            xma, ref = Path(temp) / f"s{n}.xma", Path(temp) / f"s{n}.pcm"
            with open(archive, "rb") as f:
                f.seek(offset)
                xma.write_bytes(f.read(size))
            subprocess.run([ffmpeg, "-v", "error", "-y", "-i", str(xma), "-f", "s16le", str(ref)], check=True)
            print(f"{archive.name} @ {offset:#x}:")
            result = subprocess.run([str(exe), str(xma), str(ref)], text=True, capture_output=True)
            print(result.stdout + result.stderr, end="")
            failed |= result.returncode != 0
            negative = subprocess.run([str(exe), str(xma), str(ref), "--negative-control"], text=True, capture_output=True)
            if negative.returncode == 0:
                print("FAIL negative control passed (a stalled ring went unnoticed)")
                failed = True
            else:
                print("PASS negative control: a never re-validated ring is detected")
    if failed:
        raise SystemExit(1)
    print("PASS XMA context emulation")


if __name__ == "__main__":
    main()
