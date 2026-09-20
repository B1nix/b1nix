#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Point a limine.conf's first boot entry at a different init.

    limine-set-init.py LIMINE_CONF INIT_PATH

Used while chasing a boot problem: booting the installed image with
init=/usr/local/sbin/b1nix-diag runs the kernel interfaces an init system needs
and prints one line per result, which separates a kernel refusal from a systemd
decision. The file is edited in place, so work on a copy pulled out of the ESP
with mcopy and put it back the same way.
"""
import sys

if len(sys.argv) != 3:
    sys.exit(__doc__)

path, init = sys.argv[1], sys.argv[2]
out, done = [], False
for line in open(path):
    if line.lstrip().startswith("cmdline:") and not done:
        line = line.rstrip("\n") + " init=" + init + "\n"
        done = True
    out.append(line)
if not done:
    sys.exit("no cmdline: line in " + path)
open(path, "w").writelines(out)
print("init set to", init)
