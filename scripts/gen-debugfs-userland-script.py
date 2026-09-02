#!/usr/bin/env python3
import os
import sys

if len(sys.argv) != 3:
    sys.stderr.write("usage: gen-debugfs-userland-script.py <rootfs-stage-dir> <out-script>\n")
    sys.exit(1)

stage_dir = sys.argv[1]
out_path = sys.argv[2]

fixed_dirs = [
    "/bin", "/sbin", "/etc", "/usr", "/usr/bin", "/usr/sbin",
    "/var", "/var/tmp", "/var/root", "/Core", "/Core/Servers",
]

tree_dirs = ["bin", "sbin", "etc", "usr/bin", "usr/sbin", "Core/Servers"]

lines = []
for d in fixed_dirs:
    lines.append("mkdir %s" % d)

nfiles = 0
nsyms = 0
for rel in tree_dirs:
    dpath = os.path.join(stage_dir, rel)
    if not os.path.isdir(dpath):
        continue
    for entry in sorted(os.listdir(dpath)):
        full = os.path.join(dpath, entry)
        dest = "/" + rel + "/" + entry
        if os.path.islink(full):
            target = os.readlink(full)
            lines.append("symlink %s %s" % (dest, target))
            nsyms += 1
        elif os.path.isfile(full):
            lines.append("write %s %s" % (full, dest))
            nfiles += 1

with open(out_path, "w") as f:
    f.write("\n".join(lines) + "\n")

sys.stderr.write("gen-debugfs-userland-script: %d files + %d symlinks\n" % (nfiles, nsyms))
