#!/bin/sh
# tools/copy-if-changed.sh - copy files into the staging rootfs, but only the
# ones whose contents actually differ.
#
# The root image is repacked when anything under the staging tree is newer than
# it. That test is the only thing standing between a kernel-only rebuild and a
# full `dd` + `mke2fs` of the whole tree, and a plain `cp` defeats it: copying
# identical bytes still stamps a new mtime, so the tree looked changed on every
# build and the image was rewritten every time. `cp -u` is not the fix either --
# it compares timestamps, so a file deliberately overwritten by an older source
# (musl's headers land on top of ours) would be skipped and the image would
# carry the wrong one.
#
# So: compare contents, write only on a difference, and touch nothing else.
#
# Symlinks follow cp's own rules: a plain copy dereferences, while -r and --into
# (which stand in for `cp -r` and `cp -a`) copy a link as a link. Getting that
# backwards turned /lib/ld-musl-x86_64.so.1 into a link to itself.
#
#   tools/copy-if-changed.sh SRC... DEST         # DEST is a file or a directory
#   tools/copy-if-changed.sh -r SRC... DESTDIR   # directories, copied recursively
#   tools/copy-if-changed.sh --into DIR SRC...   # DIR/<relative path of SRC>,
#                                                # for feeding a find(1) list
#
# --skip-if-in DIR leaves out any source that DIR also provides at the same
# relative path. The header staging copies ours first and musl's on top, so
# everything both of them ship was written twice on every build -- 165 files,
# each one making the staging tree look changed. Skipping them in the first
# pass leaves exactly the same headers behind, written once.
#
# --mode MODE gives the destination that mode (octal) and compares against it,
# for a file the caller chmods after copying: without it the destination never
# matches its source again and is rewritten on every build.
#
# --osabi-linux stamps EI_OSABI = ELFOSABI_LINUX (3) on what it writes, and
# ignores that byte when deciding whether anything needs writing. b1nix routes
# syscalls through its Linux translation layer for binaries marked that way, so
# the loader and a couple of packaged helpers have to carry it -- and stamping
# after an ordinary copy meant the destination never again matched its source,
# so it was rewritten, and the image behind it repacked, on every build.
#
# Exits non-zero if a source is missing, like cp.
set -eu

exec python3 - "$@" <<'PYEOF'
import os, shutil, stat, sys

argv = sys.argv[1:]
recursive = False
into = None
osabi_linux = False
skip_if_in = None
want_mode = None
while argv:
    if argv[0] == '--mode':
        if len(argv) < 2:
            sys.stderr.write("copy-if-changed: --mode needs an octal mode\n")
            sys.exit(2)
        want_mode = int(argv[1], 8)
        argv = argv[2:]
    elif argv[0] == '--osabi-linux':
        osabi_linux = True
        argv = argv[1:]
    elif argv[0] == '--skip-if-in':
        if len(argv) < 2:
            sys.stderr.write("copy-if-changed: --skip-if-in needs a directory\n")
            sys.exit(2)
        skip_if_in = argv[1]
        argv = argv[2:]
    else:
        break
if argv and argv[0] == '-r':
    recursive = True
    argv = argv[1:]
elif argv and argv[0] == '--into':
    if len(argv) < 2:
        sys.stderr.write("copy-if-changed: --into needs a directory\n")
        sys.exit(2)
    into = argv[1]
    argv = argv[2:]
if into is None and len(argv) < 2:
    sys.stderr.write("copy-if-changed: usage: [-r] SRC... DEST | --into DIR SRC...\n")
    sys.exit(2)

if into is None:
    srcs, dest = argv[:-1], argv[-1]
else:
    srcs, dest = argv, into
copied = 0

def same(src, dst, deref=False):
    try:
        ss = os.stat(src) if deref else os.lstat(src)
        ds = os.lstat(dst)
    except OSError:
        return False
    if stat.S_ISLNK(ss.st_mode) or stat.S_ISLNK(ds.st_mode):
        if not (stat.S_ISLNK(ss.st_mode) and stat.S_ISLNK(ds.st_mode)):
            return False
        return os.readlink(src) == os.readlink(dst)
    if not (stat.S_ISREG(ss.st_mode) and stat.S_ISREG(ds.st_mode)):
        return False
    if ss.st_size != ds.st_size:
        return False
    src_mode = want_mode if want_mode is not None else stat.S_IMODE(ss.st_mode)
    if src_mode != stat.S_IMODE(ds.st_mode):
        return False
    # Same size, same mode, same modification time: take it as the same file
    # rather than reading both. The package staging roots are a gigabyte and a
    # half, and comparing every byte of them on every build reads three
    # gigabytes off the disk to conclude that nothing moved. Anything that
    # writes a staged file gives it a new mtime, so a difference still shows up;
    # what this skips is the case where the answer was never in doubt.
    # CIC_PARANOID=1 compares the contents regardless.
    if (ss.st_mtime_ns == ds.st_mtime_ns and ss.st_size == ds.st_size
            and not os.environ.get('CIC_PARANOID')):
        return True
    with open(src, 'rb') as a, open(dst, 'rb') as b:
        first = True
        while True:
            x, y = a.read(1 << 20), b.read(1 << 20)
            if first and osabi_linux and x[:4] == b'\x7fELF' and len(x) > 7:
                x = x[:7] + y[7:8] + x[8:]
            first = False
            if x != y:
                return False
            if not x:
                return True

def put(src, dst, deref=False):
    global copied
    st = os.stat(src) if deref else os.lstat(src)
    if not (stat.S_ISREG(st.st_mode) or stat.S_ISLNK(st.st_mode)):
        # Devices, fifos and sockets are not staged by any of the callers, and
        # copying one would mean creating it, which needs privileges we do not
        # have. Say so rather than failing with a shutil traceback.
        sys.stderr.write(f"copy-if-changed: not a regular file or symlink: {src}\n")
        sys.exit(1)
    if same(src, dst, deref):
        return
    os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
    tmp = dst + '.cic.tmp'
    if os.path.lexists(tmp):
        os.remove(tmp)
    if os.path.islink(src) and not deref:
        os.symlink(os.readlink(src), tmp)
    else:
        shutil.copy2(src, tmp)
    if want_mode is not None and not os.path.islink(tmp):
        os.chmod(tmp, want_mode)
    if osabi_linux and not os.path.islink(tmp):
        with open(tmp, 'r+b') as f:
            head = f.read(8)
            if head[:4] != b'\x7fELF':
                sys.stderr.write(f"copy-if-changed: --osabi-linux on a non-ELF: {src}\n")
                sys.exit(1)
            if head[7] != 3:
                f.seek(7)
                f.write(bytes([3]))
    os.replace(tmp, dst)
    copied += 1
    if os.environ.get('CIC_VERBOSE'):
        sys.stderr.write(f"copy-if-changed: wrote {dst}\n")

def shadowed(rel):
    return skip_if_in is not None and os.path.exists(os.path.join(skip_if_in, rel))

def walk(src, dst, rel=''):
    for entry in os.scandir(src):
        target = os.path.join(dst, entry.name)
        child = os.path.join(rel, entry.name) if rel else entry.name
        if entry.is_dir(follow_symlinks=False):
            os.makedirs(target, exist_ok=True)
            walk(entry.path, target, child)
        else:
            if shadowed(child):
                continue
            put(entry.path, target)

deref = (not recursive) and into is None
if into is not None:
    # Each source keeps its own relative path under DIR, so a `find .`-produced
    # list rebuilds the tree rather than flattening it into one directory.
    for s in srcs:
        rel = os.path.normpath(s)
        if os.path.isabs(rel):
            sys.stderr.write(f"copy-if-changed: --into needs relative sources, got '{s}'\n")
            sys.exit(2)
        put(s, os.path.join(dest, rel))
elif len(srcs) > 1 or os.path.isdir(dest):
    for s in srcs:
        if os.path.isdir(s) and not os.path.islink(s):
            if not recursive:
                sys.stderr.write(f"copy-if-changed: -r not specified; omitting directory '{s}'\n")
                sys.exit(1)
            base = os.path.basename(s.rstrip('/'))
            target = os.path.join(dest, base)
            os.makedirs(target, exist_ok=True)
            walk(s, target, '' if base == '.' else base)
        else:
            if shadowed(os.path.basename(s)):
                continue
            put(s, os.path.join(dest, os.path.basename(s)), deref)
else:
    put(srcs[0], dest, deref)
PYEOF
