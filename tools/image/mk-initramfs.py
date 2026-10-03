#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""A gzipped newc initramfs, written without root.

    mk-initramfs.py OUT.gz --init SCRIPT [--file DEST=SRC]... [--link DEST=TARGET]...

The archive holds /init (from SCRIPT), each --file at DEST, each --link as a
symlink, the directories they sit in, /proc, and /dev/console -- a device node
a normal user cannot mknod, which is why this is not a `cpio -o` of a staged
tree. The M131 lanes use it for the guests QEMU boots inside b1nix.
"""
import argparse
import gzip
import os
import sys


def entry(name, mode, data=b"", rdev=(0, 0)):
    name = name.encode() + b"\0"
    fields = (0, mode, 0, 0, 1, 0, len(data), 0, 0, rdev[0], rdev[1], len(name), 0)
    out = ("070701" + "".join("%08X" % v for v in fields)).encode() + name
    out += b"\0" * (-len(out) % 4)
    return out + data + b"\0" * (-len(data) % 4)


def pair(text):
    dest, sep, src = text.partition("=")
    if not sep or not dest or not src:
        raise argparse.ArgumentTypeError("expected DEST=SRC, got %r" % text)
    return dest.strip("/"), src


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--init", required=True)
    ap.add_argument("--file", type=pair, action="append", default=[])
    ap.add_argument("--link", type=pair, action="append", default=[])
    args = ap.parse_args()

    dirs = {"dev", "proc"}
    for dest, _ in args.file + args.link:
        parent = os.path.dirname(dest)
        while parent:
            dirs.add(parent)
            parent = os.path.dirname(parent)

    parts = [entry(d, 0o40755) for d in sorted(dirs)]
    parts.append(entry("dev/console", 0o20600, rdev=(5, 1)))
    with open(args.init, "rb") as f:
        parts.append(entry("init", 0o100755, f.read()))
    for dest, src in args.file:
        with open(src, "rb") as f:
            data = f.read()
        mode = 0o100755 if os.access(src, os.X_OK) else 0o100644
        parts.append(entry(dest, mode, data))
    for dest, target in args.link:
        parts.append(entry(dest, 0o120777, target.encode()))
    parts.append(entry("TRAILER!!!", 0))
    blob = b"".join(parts)
    with gzip.open(args.out, "wb") as f:
        f.write(blob + b"\0" * (-len(blob) % 512))


if __name__ == "__main__":
    sys.exit(main())
