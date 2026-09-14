#!/bin/sh
# M26 kernel self-host: build the btrfs module that lets b1nix compile its own
# kernel, in-guest, with Alpine's clang and ld.lld.
#
#   sh tools/selfhost/build-selfhost-module.sh      # after `make` (x86_64)
#
# The kernel handler behind b1nix.selfhostbuild (kernel/main.c) mounts the
# module at /mnt/build and runs /mnt/build/bin/selfhost-build, which replays
# cmds.txt (one compile per line) and link.txt from /mnt/build/src.
#
# The commands are the host build's own, taken from `make -Bn`: every kernel
# and imported translation unit keeps exactly the flags it is built with on the
# host, and only the paths change. Assembly objects and the generated kallsyms
# object are staged from the host build.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH=x86_64
OUT="$ROOT_DIR/build/selfhost-out"
STAGE="$OUT/stage"
IMG="$OUT/selfhost.img"
TC="$OUT/toolchain"
SELFBUILD="$ROOT_DIR/userspace/build/$ARCH/bin/selfhost_build"

[ -f "$ROOT_DIR/build/$ARCH/kernel.elf" ] || { echo "build the kernel first (make)"; exit 1; }
[ -f "$SELFBUILD" ] || { echo "missing $SELFBUILD (make -C userspace)"; exit 1; }

echo "=== [1/4] toolchain: Alpine clang17 + lld ==="
rm -rf "$STAGE" "$TC"
mkdir -p "$STAGE/bin" "$STAGE/src" "$STAGE/obj" "$TC"
# The driver's dependency on gcc/binutils is for linking user programs; a
# kernel build compiles with -c and links with ld.lld, so none of it is needed.
B1NIX_ARCH=$ARCH ALPINE_LAYOUT=native \
	ALPINE_SKIP_DEPS="musl alpine-baselayout alpine-baselayout-data alpine-keys alpine-release busybox busybox-binsh gcc binutils libgomp libatomic gmp isl25 jansson mpc1 mpfr4 fortify-headers llvm17-linker-tools scudo-malloc" \
	sh "$ROOT_DIR/tools/packages/alpine-fetch.sh" "$TC" clang17 clang17-headers lld >/dev/null
cp -R "$TC/usr" "$STAGE/usr"
[ -d "$TC/lib" ] && cp -R "$TC/lib" "$STAGE/lib"   # zlib and friends live in /lib
# Symlinks are resolved into copies so the image does not depend on where the host staged them.
find "$STAGE/usr" "$STAGE/lib" -type l 2>/dev/null | while read -r l; do
	t="$(readlink -f "$l" 2>/dev/null || true)"
	rm -f "$l"
	if [ -d "$t" ]; then
		cp -R "$t" "$l"
	elif [ -f "$t" ]; then
		cp "$t" "$l"
	fi
done
cp "$SELFBUILD" "$STAGE/bin/selfhost-build"
RESDIR="$(cd "$STAGE" && ls -d usr/lib/llvm17/lib/clang/* | head -1)"

# The imported Linux release, as the Makefile pins it.
LV="$(sed -n 's/^LKPI_LINUX_VERSION ?= //p' "$ROOT_DIR/Makefile")"

echo "=== [2/4] sources ==="
tar -C "$ROOT_DIR" -cf - --exclude='*.o' --exclude='*.d' --exclude='.git' \
	kernel "build/src/fs-$LV" "build/src/fs-$LV-gen" "build/src/drm-core-$LV" "build/src/i915-$LV" \
	"build/$ARCH/inc" | tar -C "$STAGE/src" -xf -
find "$ROOT_DIR/build/$ARCH" -maxdepth 1 -type f \( -name '*.h' -o -name '*.inc' \) \
	-exec cp {} "$STAGE/src/build/$ARCH/" \;

echo "=== [3/4] commands from the host build ==="
make -C "$ROOT_DIR" -Bn B1CC_SYNC_CHECK=0 "build/$ARCH/kernel.elf" 2>/dev/null > "$OUT/dry.txt"
python3 - "$OUT/dry.txt" "$STAGE" "$ROOT_DIR" "$ARCH" "/mnt/build/$RESDIR" <<'PY'
import os, shlex, shutil, sys
dry, stage, root, arch, resdir = sys.argv[1:6]
bdir = f"build/{arch}/"
clang = "/mnt/build/usr/lib/llvm17/bin/clang-17"
lld = "/mnt/build/usr/bin/ld.lld"
cmds, link, objdirs, staged = [], None, set(), 0

def guest_obj(p):
    return "/mnt/build/obj/" + p[len(bdir):]

for line in open(dry):
    line = line.strip()
    if not line:
        continue
    try:
        argv = shlex.split(line)
    except ValueError:
        continue
    if argv and argv[0].endswith("ccache"):
        argv = argv[1:]
    if not argv:
        continue
    tool = os.path.basename(argv[0])
    if tool.startswith("clang") and "-c" in argv and "-o" in argv:
        out = argv[argv.index("-o") + 1]
        src = argv[argv.index("-c") + 1]
        if not out.startswith(bdir + "kernel/") and not out.startswith(bdir + "build/src/"):
            continue
        if not src.endswith(".c"):
            continue
        new = [clang, "-resource-dir", resdir]
        skip = 0
        for i, a in enumerate(argv[1:], 1):
            if skip:
                skip -= 1
                continue
            if a in ("-MMD", "-MP") or a.startswith("-ffile-prefix-map="):
                continue
            if a == "-isystem" and argv[i + 1].startswith("/usr/lib/clang/"):
                new += ["-isystem", resdir + "/include"]
                skip = 1
                continue
            if a == "-o":
                new += ["-o", guest_obj(argv[i + 1])]
                skip = 1
                continue
            new.append(a)
        cmds.append(new)
        objdirs.add(os.path.dirname(guest_obj(out)))
    elif tool == "ld.lld" and "-o" in argv and argv[argv.index("-o") + 1] == bdir + "kernel.elf":
        new = [lld]
        for a in argv[1:]:
            if a.startswith(bdir) and a.endswith(".o"):
                objdirs.add(os.path.dirname(guest_obj(a)))
                src_o = os.path.join(root, a)
                # Objects no in-guest compile produces: assembly and kallsyms.
                if not any(c[c.index("-o") + 1] == guest_obj(a) for c in cmds):
                    dst = os.path.join(stage, "obj", a[len(bdir):])
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copy(src_o, dst)
                    staged += 1
                new.append(guest_obj(a))
            elif a == bdir + "kernel.elf":
                new.append("/mnt/build/kernel.elf")
            else:
                new.append(a)
        link = new

if link is None:
    sys.exit("no final kernel.elf link command in the dry run")
for d in objdirs:
    os.makedirs(os.path.join(stage, d[len("/mnt/build/"):]), exist_ok=True)
with open(os.path.join(stage, "cmds.txt"), "w") as f:
    for c in cmds:
        f.write("\t".join(c) + "\n")
with open(os.path.join(stage, "link.txt"), "w") as f:
    f.write("\t".join(link) + "\n")
print(f"  {len(cmds)} compiles, {staged} host objects staged, link of {sum(1 for a in link if a.endswith('.o'))} objects")
PY

echo "=== [4/4] btrfs module ==="
SZ=$(du -sm "$STAGE" | cut -f1)
IMG_MB=$((SZ + SZ / 2 + 128))
rm -f "$IMG"
truncate -s "${IMG_MB}M" "$IMG"
mkfs.btrfs -q -f -m single -d single --rootdir "$STAGE" "$IMG"
echo "  $IMG = ${IMG_MB} MB"
