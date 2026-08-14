#!/bin/sh
# Build Skia (static libraries) for b1nix via cross-compilation.
#
# Produces: build/x86_64/ports/skia/install/lib/libskia.a + dependencies
#           build/x86_64/ports/skia/install/include/ (Skia public headers)
#
# Skia uses its own GN (gn/BUILDCONFIG.gn). We cross-compile using
# host clang++ with --target + --sysroot via the b1nix-cross-cc wrapper.

set -eu

B1NIX_ARCH="${B1NIX_ARCH:-x86_64}"
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
SKIA_DIR="$ROOT_DIR/build/src/skia"
BUILD_DIR="$ROOT_DIR/build/$B1NIX_ARCH/ports/skia"
INSTALL_DIR="$BUILD_DIR/install"

GN_BIN="${GN_BIN:-$(command -v gn 2>/dev/null || \
  { [ -x "$ROOT_DIR/build/x86_64/toolchain/chromium/src/buildtools/linux64/gn" ] && echo "$ROOT_DIR/build/x86_64/toolchain/chromium/src/buildtools/linux64/gn"; } || \
  { [ -x "$ROOT_DIR/build/x86_64/toolchain/v8-skeleton/gn-src/out/gn" ] && echo "$ROOT_DIR/build/x86_64/toolchain/v8-skeleton/gn-src/out/gn"; } || \
  { [ -x "$ROOT_DIR/build/src/gn-src/out/gn" ] && echo "$ROOT_DIR/build/src/gn-src/out/gn"; } || \
  echo "")}"
NINJA_BIN="${NINJA_BIN:-$(command -v ninja 2>/dev/null || echo "")}"
CROSS_CC="$ROOT_DIR/tools/ports/b1nix-cross-cc.sh"

# Ensure GN binary exists
if [ -z "$GN_BIN" ] || [ ! -x "$GN_BIN" ]; then
  echo "GN not found. Auto-building GN..." >&2
  GN_SRC="$ROOT_DIR/build/src/gn-src"
  mkdir -p "$ROOT_DIR/build/src"
  if [ ! -d "$GN_SRC" ]; then
    git clone https://gn.googlesource.com/gn "$GN_SRC" >&2
  fi
  ( cd "$GN_SRC" && python3 build/gen.py && ninja -C out gn ) >&2
  GN_BIN="$GN_SRC/out/gn"
fi

. "$ROOT_DIR/tools/toolchain/env.sh"

# Ensure cross sysroot has usr/include symlink
CROSS="$TOOLCHAIN_BUILD_HOME/cross"
AR_BIN="${AR_BIN:-$(command -v llvm-ar 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/llvm-ar 2>/dev/null || echo "$CROSS/bin/x86_64-b1nix-ar")}"
SYSROOT="$ROOT_DIR/build/$B1NIX_ARCH/ports/musl/install"
if [ ! -d "$CROSS/usr/include" ]; then
  mkdir -p "$CROSS/usr"
  ln -sfn "$SYSROOT/include" "$CROSS/usr/include" 2>/dev/null || true
fi

# Serialize concurrent invocations
mkdir -p "$BUILD_DIR" "$INSTALL_DIR/lib" "$INSTALL_DIR/include"
LOCK="$BUILD_DIR/.build-lock"
acquire_lock() {
  WAIT_COUNT=0
  while ! mkdir "$LOCK" 2>/dev/null; do
    WAIT_COUNT=$((WAIT_COUNT + 1))
    if [ -f "$LOCK/pid" ]; then
      LOCK_PID="$(cat "$LOCK/pid" 2>/dev/null || echo "")"
      if [ -n "$LOCK_PID" ] && ! kill -0 "$LOCK_PID" 2>/dev/null; then
        echo "Removing stale build lock held by dead PID $LOCK_PID..." >&2
        rm -rf "$LOCK" 2>/dev/null || true
        continue
      fi
    elif [ -d "$LOCK" ]; then
      if [ "$WAIT_COUNT" -ge 3 ]; then
        echo "Removing lock directory missing PID file..." >&2
        rm -rf "$LOCK" 2>/dev/null || true
        continue
      fi
    fi
    sleep 1
  done
  echo "$$" > "$LOCK/pid" 2>/dev/null || true
}
acquire_lock
trap 'rm -rf "$LOCK" 2>/dev/null || true' EXIT INT TERM


# --- Step 1: Fetch Skia source -----------------------------------------------
if [ ! -d "$SKIA_DIR" ]; then
  echo "Fetching Skia source..." >&2
  mkdir -p "$(dirname "$SKIA_DIR")"
  git clone --depth 1 "https://skia.googlesource.com/skia" "$SKIA_DIR" 2>&1 | tail -3
fi

# --- Step 2: Fetch Chromium //build module ------------------------------------
BUILD_SKIA="$SKIA_DIR/build"
if [ ! -d "$BUILD_SKIA" ]; then
  echo "Fetching Chromium //build module..." >&2
  git clone --depth 1 \
    "https://chromium.googlesource.com/chromium/src/build" \
    "$BUILD_SKIA" 2>&1 | tail -3
fi

# --- Step 3: Fetch third-party dependencies -----------------------------------
EXT="$SKIA_DIR/third_party/externals"
if [ ! -d "$EXT/libpng" ] || [ ! -d "$EXT/harfbuzz" ]; then
  echo "Fetching Skia third-party dependencies..." >&2
  python3 << PYEOF
import re, os, subprocess

deps_file = open('$SKIA_DIR/DEPS').read()
ext_dir = '$EXT'
os.makedirs(ext_dir, exist_ok=True)

for m in re.finditer(r'["\x27](third_party/externals/([\w-]+))["\x27]\s*:\s*["\x27](https://[^"\x27]+)@([0-9a-f]+)["\x27]', deps_file):
    name = m.group(2)
    url = m.group(3)
    commithash = m.group(4)
    target = f'{ext_dir}/{name}'
    if not os.path.exists(target):
        if name in ['v8', 'swiftshader', 'angle2']:
            print(f'  Fetching {name} (shallow, no checkout)...', flush=True)
            try:
                subprocess.run(['git', 'clone', '--depth', '1', url, target],
                              capture_output=True, timeout=120)
            except Exception as e:
                print(f'  WARNING: failed to fetch {name}: {e}')
            continue

        print(f'  Fetching {name}...', flush=True)
        try:
            subprocess.run(['git', 'clone', '--depth', '1', url, target],
                          capture_output=True, timeout=120)
            res = subprocess.run(['git', 'checkout', '--quiet', commithash], cwd=target, capture_output=True)
            if res.returncode != 0:
                subprocess.run(['git', 'fetch', '--unshallow', '--quiet'], cwd=target, capture_output=True)
                subprocess.run(['git', 'checkout', '--quiet', commithash], cwd=target, capture_output=True)
        except Exception as e:
            print(f'  WARNING: failed to fetch {name}: {e}')
PYEOF
  echo "  Fetched $(ls -d "$EXT"/*/ 2>/dev/null | wc -l) dependencies" >&2
fi

# --- Step 4: Apply b1nix patches ----------------------------------------------
echo "Applying b1nix patches..." >&2
sh "$ROOT_DIR/tools/patches/skia/apply.sh" "$SKIA_DIR"

# --- Step 5: Write GN args and generate build graph ----------------------------
# We need two builds:
#   (a) Static (is_component_build=false) → dm tool + libskia.a + Dawn
#   (b) Shared libskia.so created from the static archive
# Skia disables tools when is_component_build=true, so static-first is required.
SKIA_OUT="$SKIA_DIR/out/b1nix"
MESA_LIB_DIR="$ROOT_DIR/build/x86_64/ports/mesa/install/lib"
mkdir -p "$SKIA_OUT"

CC_WRAPPER=""
if command -v ccache >/dev/null 2>&1 && [ "${B1NIX_NO_CCACHE:-0}" != "1" ]; then
  CC_WRAPPER='cc_wrapper = "ccache"'
fi

cat > "$SKIA_OUT/args.gn" <<EOF
target_os = "b1nix"
target_cpu = "x64"
is_clang = true
$CC_WRAPPER
is_debug = false
is_official_build = false
is_component_build = false
cc = "$CROSS_CC"
cxx = "$CROSS_CC"
ar = "$AR_BIN"
skia_use_gl = true
skia_use_egl = true
skia_use_piex = false
skia_use_dng_sdk = false
skia_use_vulkan = false
skia_use_metal = false
skia_use_direct3d = false
skia_use_x11 = false
skia_use_fontconfig = true
skia_enable_graphite = true
# Dawn is built separately as libdawn_combined.a (CMake) and linked into
# smoke binaries statically. Not included in the shared libskia.so (too large).
skia_use_dawn = false
skia_use_partition_alloc = false
skia_enable_tools = true
skia_enable_skottie = true
EOF

echo "GN args written to $SKIA_OUT/args.gn" >&2

cd "$SKIA_DIR"
"$GN_BIN" gen "$SKIA_OUT" --force 2>&1

# --- Step 6: Build libskia.a (static) + Dawn static lib -----------------------
# With is_component_build=false, GN produces libskia.a (static archive).
# The smoke test links statically — no shared libs in initramfs.
echo "Building libskia.a and modules (cross-compilation, static)..." >&2
"$NINJA_BIN" -C "$SKIA_OUT" -j"$(nproc 2>/dev/null || echo 4)" \
  libskia.a skottie sksg skresources jsonreader skshaper skunicode_core skunicode_icu icu icu_bidi 2>&1

# Build Dawn separately if patches exist (CMake → libdawn_combined.a)
#
# Once, not on every build.
#
# The rebuild below throws away CMake's cache and reconfigures from scratch,
# which costs five minutes — and it was being paid by every invocation of make,
# including ones with nothing to do, because a stale-forever package rule kept
# bumping this script's prerequisites. That is 78% of the edit-build-run loop
# spent on a WebGPU implementation that neither the smoke suite nor the display
# work links against (skia_use_dawn is false above).
#
# Rebuilt when the artifact is missing, when this script is newer than it — the
# flags that need the cache thrown away live here — or on request.
dawn_needs_build() {
  [ -n "${B1NIX_REBUILD_DAWN:-}" ] && return 0
  [ -f "$SKIA_OUT/libdawn_combined.a" ] || return 0
  [ "$0" -nt "$SKIA_OUT/libdawn_combined.a" ] && return 0
  return 1
}
if [ -d "$SKIA_DIR/third_party/externals/dawn" ] && ! dawn_needs_build; then
  echo "Dawn: up to date ($SKIA_OUT/libdawn_combined.a); B1NIX_REBUILD_DAWN=1 forces a rebuild" >&2
elif [ -d "$SKIA_DIR/third_party/externals/dawn" ]; then
  #
  # The externals have to match what DEPS pins, or Dawn does not compile.
  #
  # Skia and Dawn pin SPIRV-Headers and SPIRV-Tools as a matching pair — both
  # name 29981f65 and 0d6fd73c — and this checkout had drifted from both: older
  # headers, and tools from somewhere else entirely. Tint's SPIR-V writer then
  # failed on nine identifiers the older headers predate (SPV_KHR_cooperative_matrix
  # and SPV_KHR_maximal_reconvergence), and `|| echo WARNING` hid it for as long
  # as it has been there.
  #
  # Patching the headers is not the fix and updating only one of the two is
  # worse: the pair is coupled through the grammar the tools generate their
  # tables from, so newer headers alone break SPIRV-Tools on an enumerant that
  # was renamed (LongConstantCompositeINTEL). Both are checked out at the
  # revision DEPS names, and nothing is disabled — the SPIR-V writer builds,
  # which is what Vulkan will need.
  #
  sync_external() {
    _dir="$SKIA_DIR/third_party/externals/$1"
    _rev="$2"
    [ -d "$_dir/.git" ] || return 0
    [ "$(git -C "$_dir" rev-parse HEAD 2>/dev/null)" = "$_rev" ] && return 0
    echo "Syncing $1 to the revision DEPS pins ($2)..." >&2
    git -C "$_dir" fetch --depth=50 origin "$_rev" >/dev/null 2>&1 || true
    git -C "$_dir" checkout -q "$_rev" || {
      echo "ERROR: cannot check out $1 at $_rev" >&2
      exit 1
    }
  }
  sync_external spirv-headers 29981f65241605e08b0ede4cfeb999fe3b723c6a
  sync_external spirv-tools   0d6fd73ca73830ccab5fa1f00ed5ed40124e2c55

  # The build directory keeps CMake's cache, and option() defaults never
  # override a cached value — which is how a configuration nobody asked for
  # survives a flag change. Configured from scratch so the flags below decide.
  rm -rf "$SKIA_OUT/cmake_dawn"

  echo "Building Dawn (CMake → libdawn_combined.a)..." >&2
  python3 "$SKIA_DIR/third_party/dawn/build_dawn.py" \
    --cc="$CROSS_CC" --cxx="$CROSS_CC" \
    --output_path="$SKIA_OUT/libdawn_combined.a" \
    --depfile_path="$SKIA_OUT/gen/dawn.d" \
    --gen_dir="$SKIA_OUT/gen/third_party/dawn" \
    --target_os=linux --target_cpu=x64 \
    --build_dir="$SKIA_OUT/cmake_dawn" \
    --is_clang --build_type=Release \
    --dawn_enable_opengles=true --dawn_enable_vulkan=false \
    2>&1
fi

if [ ! -f "$SKIA_OUT/libskia.a" ]; then
  echo "ERROR: libskia.a not produced" >&2
  exit 1
fi

# --- Step 7: Install ----------------------------------------------------------
echo "Installing Skia..." >&2

mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include"

# Copy static library
cp "$SKIA_OUT/libskia.a" "$INSTALL_DIR/lib/"

# Copy Dawn static lib if built
[ -f "$SKIA_OUT/libdawn_combined.a" ] && cp "$SKIA_OUT/libdawn_combined.a" "$INSTALL_DIR/lib/"

# Copy public headers
if [ -d "$SKIA_DIR/include" ]; then
  cp -r "$SKIA_DIR/include" "$INSTALL_DIR/"
fi

SIZE="$(du -sh "$INSTALL_DIR" | cut -f1)"
echo ""
echo "Skia build complete (cross-compiled for b1nix, static)."
echo "  Library: $INSTALL_DIR/lib/libskia.a"
echo "  Headers: $INSTALL_DIR/include/"
echo "  Total:   $SIZE"
echo "$INSTALL_DIR"
