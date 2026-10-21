"""Stress the production RasterPool (gpu_render.cpp) join/close protocol.

Extracts the RasterPool class verbatim and runs many back-to-back jobs of random
height (including 0-row and 1-chunk jobs that wake no helpers). Every row of
every job must run exactly once and be complete when ForRows returns; a job's
row function must never run after its ForRows returned. The negative control
removes the caller's wait for joined workers and must be detected.

  python port/tools/test_raster_pool.py [--jobs N] [--negative-control]
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "port/src/runtime/kernel/gpu_render.cpp"

HARNESS = r'''
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
int GuestFaultReport(EXCEPTION_POINTERS*) { return EXCEPTION_CONTINUE_SEARCH; }
@POOL@
int main(int argc, char** argv)
{
    const int jobs = argc > 1 ? std::atoi(argv[1]) : 200000;
    std::mt19937 rng(12345);
    std::vector<std::atomic<int>> hits(1024);
    std::atomic<int> current{ -1 }, stale{ 0 };
    long long bad = 0, rows = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int j = 0; j < jobs; ++j)
    {
        int y0 = int(rng() % 64);
        int h = (j % 7 == 0) ? int(rng() % 4) : (j % 5 == 0) ? int(rng() % 900) : int(rng() % 40);
        int y1 = std::min(1023, y0 + h);
        for (int y = y0; y < y1; ++y) hits[y].store(0, std::memory_order_relaxed);
        current.store(j, std::memory_order_release);
        std::function<void(int)> fn = [&, j](int y) {
            if (current.load(std::memory_order_acquire) != j) stale.fetch_add(1);
            hits[y].fetch_add(1, std::memory_order_relaxed);
        };
        RasterPool::Get().ForRows(y0, y1, fn);
        current.store(-1, std::memory_order_release);
        for (int y = y0; y < y1; ++y) bad += hits[y].load(std::memory_order_acquire) != 1;
        rows += y1 - y0;
    }
    // Give any straggler a chance to touch a finished job before the stale check.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("jobs=%d rows=%lld workers=%u bad_rows=%lld stale_calls=%d mean_us_per_job=%.2f\n",
                jobs, rows, RasterPool::Get().Workers(), bad, stale.load(), ms * 1000.0 / jobs);
    return bad || stale.load() ? 1 : 0;
}
'''


def extract_pool(text):
    start = text.index("class RasterPool\n")
    end = text.index("\n};\n", start) + 4
    return text[start:end]


def run(folder, name, pool, jobs):
    exe = compile_one(folder, name, HARNESS.replace("@POOL@", pool), [folder])
    result = subprocess.run([str(exe), str(jobs)], capture_output=True, text=True, timeout=300)
    return result.returncode, (result.stdout + result.stderr).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=200000)
    args = parser.parse_args()
    pool = extract_pool(SOURCE.read_text(encoding="utf-8-sig"))
    wait = "        m_doneCv.wait(lk, [&] { return m_active == 0; });\n"
    if wait not in pool:
        raise SystemExit("FAIL: expected caller wait not found in RasterPool")
    with tempfile.TemporaryDirectory(prefix="ttt2-rasterpool-") as temp:
        folder = Path(temp)
        code, output = run(folder, "pool", pool, args.jobs)
        print(output)
        if code:
            raise SystemExit("FAIL: production RasterPool lost, duplicated or leaked rows")
        print(f"PASS production RasterPool: {args.jobs} jobs, every row exactly once, no stale calls")
        broken = pool.replace(wait, "")
        code, output = run(folder, "no_wait", broken, args.jobs)
        print(output)
        if not code:
            raise SystemExit("FAIL: negative control (no wait for joined workers) was not detected")
        print("PASS negative control detected (caller not waiting for joined workers)")


if __name__ == "__main__":
    main()
