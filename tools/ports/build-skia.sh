#!/bin/sh
# Build Skia (static libraries) for b1nix via cross-compilation.
#
# Produces: build/skia-b1nix/install/lib/libskia.a + dependencies
#           build/skia-b1nix/install/include/ (Skia public headers)
#
# Skia uses its own GN (gn/BUILDCONFIG.gn). We cross-compile using
# host clang++ with --target + --sysroot via the b1nix-cross-cc wrapper.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
SKIA_DIR="$ROOT_DIR/build/ports-src/skia"
BUILD_DIR="$ROOT_DIR/build/skia-b1nix"
INSTALL_DIR="$BUILD_DIR/install"

GN_BIN="${GN_BIN:-$(command -v gn 2>/dev/null || \
  { [ -x "$ROOT_DIR/build/toolchain_build/chromium/src/buildtools/linux64/gn" ] && echo "$ROOT_DIR/build/toolchain_build/chromium/src/buildtools/linux64/gn"; } || \
  { [ -x "$ROOT_DIR/build/toolchain_build/v8-skeleton/gn-src/out/gn" ] && echo "$ROOT_DIR/build/toolchain_build/v8-skeleton/gn-src/out/gn"; } || \
  echo "/tmp/gn-bin/gn")}"
NINJA_BIN="${NINJA_BIN:-$(command -v ninja 2>/dev/null || echo "")}"
CROSS_CC="$ROOT_DIR/tools/ports/b1nix-cross-cc.sh"

# Ensure GN binary exists
if [ ! -x "$GN_BIN" ]; then
  echo "ERROR: GN not found. Install gn or set GN_BIN." >&2
  echo "  Download: https://chrome-infra-packages.appspot.com/dl/gn/gn/linux-amd64/+/latest" >&2
  exit 1
fi

# Ensure cross sysroot has usr/include symlink
CROSS="$ROOT_DIR/build/toolchain_build/x86_64-b1nix/cross"
if [ ! -d "$CROSS/usr/include" ]; then
  mkdir -p "$CROSS/usr"
  ln -sfn "$CROSS/x86_64-b1nix/include" "$CROSS/usr/include"
fi

. "$ROOT_DIR/tools/toolchain/env.sh"

# Serialize concurrent invocations
mkdir -p "$BUILD_DIR" "$INSTALL_DIR/lib" "$INSTALL_DIR/include"
LOCK="$BUILD_DIR/.build-lock"
while ! mkdir "$LOCK" 2>/dev/null; do sleep 1; done
trap 'rmdir "$LOCK" 2>/dev/null || true' EXIT INT TERM


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
MESA_LIB_DIR="$ROOT_DIR/build/mesa-b1nix/x86_64-b1nix/install/lib"
mkdir -p "$SKIA_OUT"

cat > "$SKIA_OUT/args.gn" << EOF
target_os = "linux"
target_cpu = "x64"
is_debug = false
is_official_build = false
is_component_build = false
cc = "$CROSS_CC"
cxx = "$CROSS_CC"
ar = "$CROSS/bin/x86_64-b1nix-ar"
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
skia_use_system_zlib = false
skia_use_system_libpng = false
skia_use_system_libjpeg_turbo = false
skia_use_system_expat = false
skia_use_system_harfbuzz = false
skia_use_system_icu = false
skia_use_system_freetype2 = false
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
if [ -d "$SKIA_DIR/third_party/externals/dawn" ]; then
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
    2>&1 || echo "WARNING: Dawn build failed" >&2
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
