"""Exercise the real batch launcher against a tiny fake executable, never the game.

The child verifies that optional logging gives it native disk handles, captures
both streams, and preserves its exit code. Missing inputs and unusable log paths
must fail before the fake executable is started.
"""
from pathlib import Path
import ctypes
import os
import shutil
import subprocess
import tempfile

from test_vcmpgtuh import ROOT, compile_one

FAKE = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    if (const char* marker = std::getenv("TTT2_TEST_MARKER")) {
        FILE* f = std::fopen(marker, "wb"); if (!f) return 91;
        std::fputs("launched", f); std::fclose(f);
    }
    char cwd[4096]{}; GetCurrentDirectoryA(sizeof(cwd), cwd);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::printf("stdout-type=%lu\n", GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)));
    std::fprintf(stderr, "stderr-type=%lu\n", GetFileType(GetStdHandle(STD_ERROR_HANDLE)));
    std::printf("argc=%d\nimage=%s\ncwd=%s\nvfs=%s\n", argc,
        argc > 1 ? argv[1] : "", cwd, std::getenv("TTT2_VFS_ROOT"));
    for (int i=0; i<100; ++i) std::fprintf(stderr, "trace-%d\n", i);
    return 37;
}
'''


def main():
    checks = 0

    def check(condition, message):
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError(message)

    with tempfile.TemporaryDirectory(prefix="ttt2-launch-") as temp:
        folder = Path(temp)
        project = folder / "project with spaces & punctuation"
        caller = folder / "caller with spaces"
        caller.mkdir()
        (project / "tools").mkdir(parents=True)
        image = project / "re/decrypted/Default_patched_tu3.bin"
        image.parent.mkdir(parents=True)
        image.write_bytes(b"synthetic fixture, not game data")
        archive = project / "re/game_files_x/data000.bin"
        archive.parent.mkdir(parents=True)
        archive.write_bytes(b"synthetic archive marker")
        fake = compile_one(folder, "fake_port", FAKE, [])
        for build in ("build", "build-o1"):
            destination = project / "port" / build / "tekken_recomp.exe"
            destination.parent.mkdir(parents=True)
            shutil.copyfile(fake, destination)
        launcher = project / "tools/run_port.cmd"
        shutil.copyfile(ROOT / "tools/run_port.cmd", launcher)
        marker = caller / "started.marker"
        env = dict(os.environ, TTT2_TEST_MARKER=str(marker))

        def run(*args, script=launcher):
            marker.unlink(missing_ok=True)
            # cmd /s consumes one outer quote pair. All remaining arguments are
            # fixture-owned paths, individually quoted for cmd rather than CRT.
            words = " ".join('"' + str(value) + '"' for value in (script, *args))
            command = 'cmd.exe /d /s /c "' + words + '"'
            return subprocess.run(command,
                                  cwd=caller, env=env, capture_output=True,
                                  text=True, timeout=10)

        result = run("build-o1", "native log & trace.txt")
        log = caller / "native log & trace.txt"
        check(result.returncode == 37, "native child exit code lost: " + repr(result))
        check(marker.exists() and log.exists(), "logged launch missing")
        data = log.read_text()
        check("stdout-type=1" in data and "stderr-type=1" in data,
              "child stdout/stderr must be disk handles, not relay pipes")
        check("trace-0\n" in data and "trace-99\n" in data, "stderr incomplete")
        check("argc=2" in data and "image=" + str(image) in data, "image argument incorrect")
        check("cwd=" + str(project / "port") in data, "working directory incorrect")
        check("vfs=" + str(archive.parent) in data, "expanded archive root incorrect")
        check(not result.stdout and not result.stderr, "logged child leaked into caller pipe")
        log.write_text("old log sentinel")
        check(run("build-o1", log).returncode == 37, "absolute log path failed")
        check("old log sentinel" not in log.read_text(), "log should replace previous run")

        result = run()
        check(result.returncode == 37 and marker.exists(), "default launch broken")
        check("stdout-type=3" in result.stdout and "stderr-type=3" in result.stderr,
              "unlogged launch must retain caller streams")
        for name, arguments in (
            ("missing build", ("absent-build", log)),
            ("missing log parent", ("build-o1", "absent-parent/log.txt")),
            ("directory as log", ("build-o1", caller)),
        ):
            result = run(*arguments)
            check(result.returncode != 0, name + " succeeded: " + repr(result))
            check(not marker.exists(), name + " started executable")
        readonly = caller / "readonly.txt"
        readonly.write_text("preserve me")
        attributes = ctypes.windll.kernel32.SetFileAttributesW
        attributes.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32]
        if not attributes(str(readonly), 1):
            raise ctypes.WinError()
        try:
            result = run("build-o1", readonly)
            check(result.returncode != 0 and not marker.exists(), "readonly log started executable")
            check(readonly.read_text() == "preserve me", "readonly log changed")
        finally:
            attributes(str(readonly), 0x80)
        image.rename(image.with_suffix(".fixture"))
        result = run("build-o1", log)
        check(result.returncode != 0 and not marker.exists(), "missing image started executable")
        image.with_suffix(".fixture").rename(image)
        archive.rename(archive.with_suffix(".fixture"))
        result = run("build-o1", log)
        check(result.returncode != 0 and not marker.exists(), "missing archives started executable")
        archive.with_suffix(".fixture").rename(archive)

        negative = launcher.with_name("without_native_redirection.cmd")
        source = launcher.read_text()
        changed = source.replace(' >"%TTT2_RUN_LOG%" 2>&1', "")
        check(changed != source, "negative control did not alter launcher")
        negative.write_text(changed)
        result = run("build-o1", "negative.txt", script=negative)
        check(result.returncode == 37 and "stderr-type=3" in result.stderr and
              not (caller / "negative.txt").read_bytes(), "missing-redirection control undetected")
    print(f"PASS {checks} native launcher checks; missing-redirection negative control detected")


if __name__ == "__main__":
    main()
