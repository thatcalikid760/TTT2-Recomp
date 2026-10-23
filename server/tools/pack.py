"""pack.py <folder> <zip|tar.gz>: archive a release folder (release.sh) with fixed modes - the program and the
ttt2admin wrapper executable, everything else 0644, owner root - so a Linux archive made on Windows still runs after
extracting."""
import os
import shutil
import sys
import tarfile

folder, kind = sys.argv[1].rstrip("/\\"), sys.argv[2]
name = os.path.basename(folder)
if kind == "zip":
    shutil.make_archive(folder, "zip", os.path.dirname(folder) or ".", name)
    sys.exit(0)


def fix(info):
    info.uid = info.gid = 0
    info.uname = info.gname = "root"
    executable = info.isdir() or os.path.basename(info.name) in ("ttt2-server", "ttt2admin")
    info.mode = 0o755 if executable else 0o644
    return info


with tarfile.open(folder + ".tar.gz", "w:gz") as t:
    t.add(folder, arcname=name, filter=fix)
