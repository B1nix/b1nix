#!/bin/sh
# Fail the build when a statically linked executable lands in the rootfs.
#
# b1nix links everything dynamically against the one-blob musl libc.so, the way
# every mainstream distribution does. Static binaries are the exception, not a
# shortcut: they duplicate libc in every image, and they carry no PT_INTERP, so
# the loader has to guess their ABI from EI_OSABI instead of reading their
# interpreter (that guess is what made openrc-init #GP in ring 3 until the port
# started stamping the OSABI byte by hand).
#
# Classification is by ELF type, not by file name:
#   ET_EXEC                      -> statically linked program   (rejected)
#   ET_DYN with PT_INTERP        -> dynamic PIE program         (fine)
#   ET_DYN without PT_INTERP     -> shared library, or a deliberate static-PIE
#   ET_REL                       -> object file (crt0.o and friends)
#
# Exceptions live in tools/configs/static-allowlist.txt, one path per line with
# a reason after '#'. An entry without a reason is itself an error: making
# something static must be a visible, argued decision in a reviewed file, not a
# quiet `-static` inside a port script.
#
# Usage: sh tools/check-dynamic.sh [rootfs-dir]

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
[ -f "$PROJECT_DIR/Makefile" ] || PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ARCH="${ARCH:-x86_64}"
ROOTFS="${1:-$PROJECT_DIR/build/$ARCH/rootfs}"
ALLOWLIST="$PROJECT_DIR/tools/configs/static-allowlist.txt"

[ -d "$ROOTFS" ] || { echo "check-dynamic: no rootfs at $ROOTFS — nothing to check"; exit 0; }

PY=$(command -v python3 || true)
[ -n "$PY" ] || { echo "check-dynamic: python3 not found, skipping"; exit 0; }

"$PY" - "$ROOTFS" "$ALLOWLIST" <<'PYEOF'
import os, struct, sys

rootfs, allowlist_path = sys.argv[1], sys.argv[2]

ET_EXEC, ET_DYN, PT_INTERP = 2, 3, 3

allowed, malformed = {}, []
if os.path.exists(allowlist_path):
    for lineno, raw in enumerate(open(allowlist_path), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        path, _, reason = line.partition('#')
        path, reason = path.strip(), reason.strip()
        if not reason:
            malformed.append((lineno, path))
        allowed[path] = reason

def elf_kind(path):
    """(e_type, has_interp) or None when the file is not a 64-bit ELF."""
    try:
        with open(path, 'rb') as f:
            hdr = f.read(64)
            if hdr[:4] != b'\x7fELF' or len(hdr) < 64:
                return None
            e_type = struct.unpack_from('<H', hdr, 16)[0]
            phoff = struct.unpack_from('<Q', hdr, 32)[0]
            phentsize = struct.unpack_from('<H', hdr, 54)[0]
            phnum = struct.unpack_from('<H', hdr, 56)[0]
            if not phnum or not phentsize:
                return (e_type, False)
            f.seek(phoff)
            ph = f.read(phentsize * phnum)
            types = {struct.unpack_from('<I', ph, i * phentsize)[0]
                     for i in range(phnum)}
            return (e_type, PT_INTERP in types)
    except (OSError, struct.error):
        return None

offenders, used = [], set()
for dirpath, _, filenames in os.walk(rootfs):
    for name in filenames:
        full = os.path.join(dirpath, name)
        if os.path.islink(full):
            continue
        kind = elf_kind(full)
        if kind is None:
            continue
        e_type, has_interp = kind
        # Only fully linked programs are judged. ET_REL (.o, crt files) and
        # ET_DYN without PT_INTERP (shared libraries, and the odd deliberate
        # static-PIE) are not statically linked executables.
        if e_type != ET_EXEC:
            continue
        rel = full[len(rootfs):]
        if rel in allowed:
            used.add(rel)
            continue
        offenders.append((rel, os.path.getsize(full)))

rc = 0
if malformed:
    rc = 1
    print("check-dynamic: allowlist entries without a reason "
          "(state WHY the binary must be static):", file=sys.stderr)
    for lineno, path in malformed:
        print(f"  {allowlist_path}:{lineno}: {path}", file=sys.stderr)

stale = sorted(set(allowed) - used)
if stale:
    print("check-dynamic: allowlist entries that no longer exist "
          "(drop them):", file=sys.stderr)
    for path in stale:
        print(f"  {path}", file=sys.stderr)

if offenders:
    rc = 1
    total = sum(size for _, size in offenders)
    print(f"check-dynamic: {len(offenders)} statically linked "
          f"executable(s) in the rootfs ({total // 1024} KiB):", file=sys.stderr)
    for rel, size in sorted(offenders):
        print(f"  {size // 1024:6d}K  {rel}", file=sys.stderr)
    print("", file=sys.stderr)
    print("Link them against the shared libc instead — see any of the port", file=sys.stderr)
    print("scripts under tools/ports/ for the flags. If a binary genuinely has", file=sys.stderr)
    print(f"to be static, add it to {allowlist_path} WITH a reason.", file=sys.stderr)

if rc == 0:
    print(f"check-dynamic: ok — no unexpected static executables "
          f"({len(allowed)} allowlisted)")
sys.exit(rc)
PYEOF
