#!/bin/sh
# Build isolation for parallel agents/tasks.
#
# Run this INSIDE a git worktree (e.g. one created by an agent's
# `isolation: "worktree"`). It symlinks the expensive, task-independent build
# artifacts from the MAIN worktree into this one — the cross/native toolchains
# (~55G build/toolchain_build), the Rust+LLVM trees (~12G build/rust-native,
# build/rust, build/rust-out), and every built port — so this worktree REUSES
# them instead of rebuilding 67G from scratch.
#
# The per-arch kernel/initramfs/ISO output (build/x86_64, build/x86) is kept
# LOCAL, so this worktree's kernel build never touches the main build. An agent
# can then `make ARCH=x86_64 iso` / `sh tests/smoke.sh x86_64` here completely
# isolated from main, while the heavy toolchain stays shared and read-only.
#
# (Same-tree isolation without a worktree: use `make BUILD_ROOT=build-<task> …`.)
set -eu

WT="$(git rev-parse --show-toplevel)"
MAIN="$(git worktree list --porcelain | awk '/^worktree /{print substr($0,10); exit}')"

if [ "$WT" = "$MAIN" ]; then
	echo "share-toolchain: refusing — '$WT' is the MAIN worktree (run this in an agent worktree)." >&2
	exit 1
fi
if [ ! -d "$MAIN/build" ]; then
	echo "share-toolchain: main worktree has no build/ yet ($MAIN/build) — build it there first." >&2
	exit 1
fi

mkdir -p "$WT/build"
shared=0
for d in "$MAIN/build"/*; do
	[ -e "$d" ] || continue
	name="$(basename "$d")"
	case "$name" in
	x86_64 | x86)
		# per-arch kernel/initramfs/ISO output — keep LOCAL to this worktree
		continue
		;;
	esac
	# Don't clobber anything already present locally.
	[ -e "$WT/build/$name" ] || [ -L "$WT/build/$name" ] && continue
	ln -s "$d" "$WT/build/$name"
	echo "  shared  build/$name -> $d"
	shared=$((shared + 1))
done

echo "share-toolchain: linked $shared shared dir(s) from $MAIN/build"
echo "  build/<arch> stays LOCAL — your kernel/ISO build won't touch main."
echo "  now: make ARCH=x86_64 iso   (reuses the shared toolchain/ports)"
