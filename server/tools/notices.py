"""Write THIRD_PARTY_NOTICES.txt: the licence of every module compiled into ttt2-server (and of Go itself), for
the release folder next to the binaries. Run from server/ after changing dependencies:

    python tools/notices.py <path to go> > THIRD_PARTY_NOTICES.txt
"""
import os
import subprocess
import sys

go = sys.argv[1] if len(sys.argv) > 1 else "go"
env = dict(os.environ, GOTOOLCHAIN="local", GOPROXY="off")
cache = subprocess.check_output([go, "env", "GOMODCACHE"], env=env, text=True).strip()
goroot = subprocess.check_output([go, "env", "GOROOT"], env=env, text=True).strip()
mods = set()
for goos in ("windows", "linux"):
    out = subprocess.check_output(
        [go, "list", "-deps", "-f", "{{with .Module}}{{.Path}}@{{.Version}}{{end}}", "./cmd/ttt2-server"],
        env=dict(env, GOOS=goos, CGO_ENABLED="0"), text=True)
    mods.update(m for m in out.split() if m and not m.startswith("ttt2-server@"))


def licence(folder):
    for name in sorted(os.listdir(folder)):
        if name.upper().startswith(("LICENSE", "LICENCE", "COPYING")):
            return open(os.path.join(folder, name), encoding="utf-8", errors="replace").read().strip()
    raise SystemExit("no licence file in " + folder)


print("ttt2-server includes the following software. Their licences ask for these notices to travel with it.")
print("SQLite itself (inside modernc.org/sqlite) is in the public domain.\n")
sections = [("The Go programming language (runtime and standard library)", licence(goroot))]
for mod in sorted(mods):
    path, version = mod.split("@", 1)
    folder = os.path.join(cache, *path.split("/")) + "@" + version
    sections.append((f"{path} {version}", licence(folder)))
for title, text in sections:
    print("=" * 100)
    print(title)
    print("=" * 100)
    print(text + "\n")
