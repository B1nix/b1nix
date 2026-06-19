#!/bin/sh
# Build the `gn` meta-build tool from source — prerequisite for `gn gen` on the
# V8 → b1nix skeleton (see tools/patches/v8/PORT-PLAN.md). Run this yourself: it
# clones + builds external code, which Claude isn't allowed to do unattended.
# Takes ~1-2 min; needs git + python3 + g++ + ninja (all present on this box).
#
#   sh tools/build-gn.sh
#
# Result: build/toolchain_build/v8-skeleton/gn-src/out/gn  (gitignored, cached;
# survives `make clean`, which spares build/toolchain_build/).
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR/build/toolchain_build/v8-skeleton"
# Full clone (NOT --depth 1): build/gen.py runs `git describe --match
# initial-commit` to stamp the version, and a shallow clone has no tags.
# The gn repo is tiny (~1.4 MiB), so a full clone is cheap. Re-clone if the
# existing checkout is the broken shallow one (missing the initial-commit tag).
if [ -d gn-src ] && ! git -C gn-src describe HEAD --match initial-commit >/dev/null 2>&1; then
  echo "removing shallow/tag-less gn-src and re-cloning full..."
  rm -rf gn-src
fi
[ -d gn-src ] || git clone https://gn.googlesource.com/gn gn-src
cd gn-src
python3 build/gen.py
ninja -C out gn
echo "=== gn built: $(pwd)/out/gn ==="
./out/gn --version
