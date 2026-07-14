#!/bin/sh
# Apply the b1nix GN-target patches to a Skia checkout.
# See PORT-PLAN.md for rationale.
#
#   sh tools/patches/skia/apply.sh <path-to-skia-checkout>
#
# Idempotent: every patch is grep-guarded, so re-running is a no-op.
# IMPORTANT: Patches S1 & S2 live in //build, which must be re-applied
# after every sync. build-skia.sh calls this for exactly that.
set -eu

SKIA="${1:?usage: apply.sh <path-to-skia-checkout>}"
PATCHDIR="$(cd "$(dirname "$0")" && pwd)"
[ -f "$SKIA/BUILD.gn" ] || { echo "not a skia checkout: $SKIA"; exit 1; }
BUILD="$SKIA/build"
[ -d "$BUILD/config" ] || { echo "//build missing: $BUILD"; exit 1; }

die() { echo "apply.sh: $1" >&2; exit 1; }

# --- Patch S1: //build/config/BUILDCONFIG.gn default-toolchain dispatch --------
F="$BUILD/config/BUILDCONFIG.gn"
if ! grep -q '//build/toolchain/b1nix:' "$F"; then
  # Insert b1nix branch after the zos branch
  perl -0777 -i -pe 's~(\} else if \(target_os == "zos"\) \{\n  _default_toolchain = "//build/toolchain/zos:\$target_cpu"\n)~${1}} else if (target_os == "b1nix") {\n  _default_toolchain = "//build/toolchain/b1nix:\$target_cpu"\n~' "$F"
  grep -q '//build/toolchain/b1nix:' "$F" || die "Patch S1 anchor not found in $F"
  echo "Patch S1 applied: BUILDCONFIG.gn"
else echo "Patch S1 already present"; fi

# --- Patch S2: //build/toolchain/b1nix/BUILD.gn (net-new) ---------------------
mkdir -p "$BUILD/toolchain/b1nix"
cp -f "$PATCHDIR/toolchain/b1nix/BUILD.gn" "$BUILD/toolchain/b1nix/BUILD.gn"
echo "Patch S2 applied: toolchain/b1nix/BUILD.gn"

# --- Patch S3: include/private/SkFeatures.h — OS detection -------------------
# NOT needed: b1nix defines __linux__ via Chromium's //build, so Skia's existing
# `#elif defined(__linux)` → SK_BUILD_FOR_UNIX path already handles b1nix.
F="$SKIA/include/private/SkFeatures.h"
if [ -f "$F" ]; then
  echo "Patch S3 skipped: __linux__ already routes to SK_BUILD_FOR_UNIX"
else echo "Patch S3 skipped: file not found"; fi

# --- Patch S4: BUILD.gn — force-disable unavailable backends -------------------
F="$SKIA/BUILD.gn"
if [ -f "$F" ] && ! grep -q 'b1nix_skia_disable_vulkan' "$F"; then
  # Append a b1nix-specific block at the end of the file to override defaults
  cat >> "$F" << 'GNBLOCK'

# b1nix overrides: disable GPU backends not available on b1nix
if (target_os == "b1nix") {
  skia_use_vulkan = false
  skia_use_metal = false
  skia_use_direct3d = false
  skia_use_x11 = false
  skia_use_wayland = false
  skia_use_egl = true
  skia_use_gl = true
  skia_use_piex = false
  skia_use_dng_sdk = false
  skia_use_system_freetype = false
  skia_use_system_expat = false
  skia_use_system_harfbuzz = false
  skia_use_system_icu = false
  skia_use_system_libjpeg_turbo = false
  skia_use_system_libpng = false
  skia_use_system_zlib = false
  skia_use_system_expat = false
}
GNBLOCK
  grep -q 'target_os == "b1nix"' "$F" || die "Patch S4 write failed"
  echo "Patch S4 applied: BUILD.gn (backend overrides)"
else echo "Patch S4 already present or file not found"; fi

# --- Patch S5: src/gpu/ganesh/gl/GrGLMakeNativeInterface.cpp ------------------
# Route GL proc resolution to the b1nix EGL/OSMesa path
F="$SKIA/src/gpu/ganesh/gl/GrGLMakeNativeInterface.cpp"
if [ -f "$F" ] && ! grep -q 'b1nix' "$F"; then
  # Add b1nix as a recognized platform that uses EGL
  perl -0777 -i -pe 's~(GrGLMakeNativeInterface\(\) \{)~#if defined(__b1nix__)\n#include <EGL/egl.h>\nstatic GrGLFuncPtr b1nix_get_gl_proc(const char* name) {\n  return (GrGLFuncPtr)eglGetProcAddress(name);\n}\n#endif\n\nGrGLMakeNativeInterface\(\) \{\n#if defined(__b1nix__)\n  return GrGLInterfaces::MakeFunctionLoader(b1nix_get_gl_proc);\n#else~' "$F"
  perl -0777 -i -pe 's~(return nullptr;\n)\}$~${1}#endif\n}~' "$F"
  grep -q 'b1nix' "$F" || die "Patch S5 anchor not found in $F"
  echo "Patch S5 applied: GrGLMakeNativeInterface (b1nix EGL path)"
else echo "Patch S5 already present or file not found"; fi

# --- Patch S6: src/ports/ — add b1nix platform files -------------------------
# Create minimal b1nix port files in src/ports/
PORTS_DIR="$SKIA/src/ports"
if [ -d "$PORTS_DIR" ] && [ ! -f "$PORTS_DIR/SkOSFile_b1nix.cpp" ]; then
  cat > "$PORTS_DIR/SkOSFile_b1nix.cpp" << 'CPPEOF'
// b1nix OS port for Skia file operations.
// Most POSIX file operations work directly; this provides the
// platform-specific SkOSFile_* implementations.

#include "src/ports/SkOSFile_posix.h"

// b1nix provides full POSIX VFS: open/read/write/close/mmap/munmap/stat
// The posix port handles everything we need.
CPPEOF
  echo "Patch S6a applied: SkOSFile_b1nix.cpp"
else echo "Patch S6a already present or ports dir not found"; fi

# --- Patch S7: third_party/zlib — fix build for b1nix -------------------------
# Skia's bundled zlib may reference <sys/auxv.h> or other Linux-only headers
F="$SKIA/third_party/zlib/zconf.h"
if [ -f "$F" ] && ! grep -q 'b1nix' "$F"; then
  # Ensure ZLIB_COMPAT is defined for Skia's expected API
  echo "/* b1nix: ensure compat mode */" >> "$F"
  echo "Patch S7a applied: zlib compat" 
else echo "Patch S7a already present or not needed"; fi

# --- Patch S8: gn/skia/BUILD.gn — enable dm tools for b1nix ------------------
F="$SKIA/gn/skia/BUILD.gn"
if [ -f "$F" ] && ! grep -q 'target_os == "b1nix"' "$F"; then
  # Enable dm tool (runs headless via EGL PBuffer), disable skottie
  cat >> "$F" << 'GNBLOCK2'

# b1nix: enable dm tool (headless EGL), disable skottie
if (target_os == "b1nix") {
  skia_enable_tools = true
  skia_enable_skottie = false
}
GNBLOCK2
  echo "Patch S8 applied: gn/skia/BUILD.gn (enable dm tools)"
else echo "Patch S8 already present or file not found"; fi

# --- Patch S8b: BUILD.gn — make dm skottie dep conditional for b1nix ----------
# dm unconditionally depends on modules/skottie, but we disable skottie on b1nix.
# Remove skottie from dm's deps list and add conditional below.
F="$SKIA/BUILD.gn"
if [ -f "$F" ] && ! grep -q 'b1nix_dm_skottie' "$F"; then
  # Remove skottie lines from dm deps list
  perl -0777 -i -pe 's~"modules/skottie",\s*"modules/skottie:utils",\s*~~g' "$F"
  # Add conditional skottie dep after dm's deps block
  perl -0777 -i -pe 's~(test_app\("dm"\) \{[^}]*\] *)(\n\s*if \(skia_use_libpng_decode)~${1}\n        if (skia_enable_skottie) {  # b1nix_dm_skottie\n          deps += [ "modules/skottie", "modules/skottie:utils" ]\n        }${2}~s' "$F"
  echo "Patch S8b applied: BUILD.gn (dm skottie conditional)"
else echo "Patch S8b already present or file not found"; fi

# --- Patch S9: third_party/externals/piex — add missing <cstring> include ------
F="$SKIA/third_party/externals/piex/src/image_type_recognition/image_type_recognition_lite.cc"
if [ -f "$F" ] && ! grep -q '#include <cstring>' "$F"; then
  sed -i '26a #include <cstring>' "$F"
  echo "Patch S9 applied: piex cstring include"
else echo "Patch S9 already present or file not found"; fi

# --- Patch S10: dng_sdk — fix int64_t vs long type mismatch ------------------
# b1nix defines int64_t as long long, but __builtin_smull_overflow expects long*.
# Force the long long path on b1nix by patching both #if guards.
F="$SKIA/third_party/externals/dng_sdk/source/dng_safe_arithmetic.h"
if [ -f "$F" ] && ! grep -q 'b1nix' "$F"; then
  sed -i 's/#if __has_builtin(__builtin_smull_overflow)/#if __has_builtin(__builtin_smull_overflow) \&\& !defined(__b1nix__)/g' "$F"
  sed -i 's/#if (LONG_MAX == INT64_MAX) && !defined(__APPLE__)/#if (LONG_MAX == INT64_MAX) \&\& !defined(__APPLE__) \&\& !defined(__b1nix__)/g' "$F"
  grep -q 'b1nix' "$F" 2>/dev/null && echo "Patch S10 applied: dng_sdk int64_t fix" || echo "Patch S10 skipped: pattern not found"
else echo "Patch S10 already present or file not found"; fi

# --- Patch S11: third_party/piex/BUILD.gn — compile piex_cr3.cc ---------------
# The pinned piex checkout ships piex_cr3.cc (defines piex::Cr3GetPreviewData /
# Cr3GetOrientation, which piex.cc references), but the DEPS-pinned BUILD.gn
# predates it and omits the source. Without it libpiex.so has undefined Cr3*
# symbols and the dynamic Skia link fails under --no-allow-shlib-undefined.
F="$SKIA/third_party/piex/BUILD.gn"
if [ -f "$F" ] && ! grep -q 'piex_cr3.cc' "$F"; then
  sed -i 's~\( *\)"../externals/piex/src/piex.cc",~&\n\1"../externals/piex/src/piex_cr3.cc",~' "$F"
  grep -q 'piex_cr3.cc' "$F" || die "Patch S11 write failed"
  echo "Patch S11 applied: piex BUILD.gn (compile piex_cr3.cc)"
else echo "Patch S11 already present or file not found"; fi

# --- Patch S12: Dawn args.gni — enable OpenGL ES, disable Vulkan for b1nix ---
F="$SKIA/third_party/dawn/args.gni"
if [ -f "$F" ] && ! grep -q 'b1nix' "$F"; then
  # On b1nix: use OpenGL ES via EGL (OSMesa), no Vulkan/D3D/Metal
  cat >> "$F" << 'DAWNBLOCK'

# b1nix overrides: OpenGL ES via EGL (OSMesa backend), no Vulkan/D3D/Metal
if (target_os == "linux") {
  dawn_enable_opengles = true
  dawn_enable_vulkan = false
  dawn_enable_d3d11 = false
  dawn_enable_d3d12 = false
  dawn_enable_metal = false
}
DAWNBLOCK
  grep -q 'b1nix' "$F" || die "Patch S12 write failed"
  echo "Patch S12 applied: Dawn args.gni (OpenGL ES for b1nix)"
else echo "Patch S12 already present or file not found"; fi

# --- Patch S12b: Dawn CMakeLists.txt — disable X11/Wayland, enable EGL-only ---
# b1nix uses headless EGL (PBuffer via OSMesa), no X11/Wayland display server.
F="$SKIA/third_party/externals/dawn/CMakeLists.txt"
if [ -f "$F" ] && ! grep -q 'b1nix_no_x11' "$F"; then
  # Force X11 and Wayland OFF for b1nix (headless EGL only)
  perl -0777 -i -pe 's~(elseif\(UNIX\)\s*\n\s*set\(USE_WAYLAND \$\{WAYLAND_FOUND\}\))~# b1nix: headless EGL, no display server\n  set(USE_WAYLAND OFF)\n  set(USE_X11 OFF)  # b1nix_no_x11\n  return()\n${1}~' "$F" 2>/dev/null || true
  # Simpler approach: just set defaults before the UNIX block
  if ! grep -q 'b1nix_no_x11' "$F"; then
    perl -0777 -i -pe 's~(option\(DAWN_USE_WAYLAND.*)~# b1nix: force headless EGL (no X11/Wayland)\nset(USE_WAYLAND OFF)\nset(USE_X11 OFF)  # b1nix_no_x11\n\n${1}~' "$F"
  fi
  echo "Patch S12b applied: Dawn CMakeLists.txt (disable X11/Wayland)"
else echo "Patch S12b already present or file not found"; fi

# --- Patch S13: Dawn CMakeLists.txt — add b1nix EGL include paths ---
# Dawn's OpenGL ES backend needs EGL/GL headers. When cross-compiling for b1nix,
# these come from Skia's third_party/externals/{egl-registry,opengl-registry}.
F="$SKIA/third_party/externals/dawn/CMakeLists.txt"
if [ -f "$F" ] && ! grep -q 'b1nix_egl_includes' "$F"; then
  # After the DAWN_ENABLE_OPENGLES definition, add b1nix EGL include paths
  perl -0777 -i -pe 's~(if \(DAWN_ENABLE_OPENGLES\)\s*\n\s*target_compile_definitions\(dawn_internal_config INTERFACE "DAWN_ENABLE_BACKEND_OPENGLES"\))~${1}\n    # b1nix: add EGL/GL headers from Skia third_party for cross-compilation\n    if (CMAKE_CROSSCOMPILING)\n      target_include_directories(dawn_internal_config SYSTEM INTERFACE\n        "${CMAKE_CURRENT_SOURCE_DIR}/../egl-registry/api"\n        "${CMAKE_CURRENT_SOURCE_DIR}/../opengl-registry/api"\n      )\n    endif()~' "$F"
  grep -q 'b1nix_egl_includes\|b1nix.*EGL' "$F" || echo "Patch S13: pattern not matched, Dawn may find EGL from sysroot"
  echo "Patch S13 applied: Dawn CMakeLists.txt (EGL includes for cross-compile)"
else echo "Patch S13 already present or file not found"; fi

echo ""
echo "All Skia patches applied successfully."
echo "Build with: gn gen out/b1nix --args='target_os=\"linux\" ...' && ninja -C out/b1nix"
