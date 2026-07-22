#!/bin/sh
# b1nix cross-compiler wrapper for Skia GN builds.
# Calls host clang/clang++ with --target + --sysroot for cross-compilation.
# Handles compilation AND linking: when -shared or -o linking is detected,
# uses ld.lld with b1nix sysroot instead of host linker.

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
CROSS="$ROOT_DIR/build/${B1NIX_ARCH:-x86_64}/toolchain/llvm/cross"
CLANG="${B1NIX_CLANG:-clang}"
CLANGXX="${B1NIX_CLANGXX:-clang++}"
LLD="${B1NIX_LLD:-$(command -v ld.lld 2>/dev/null || echo /usr/bin/ld.lld)}"

# Feature detection intercepts for GN
if [ "$1" = "--version" ]; then
  echo "clang version 22.1.6 (b1nix cross)"
  echo "Target: x86_64-unknown-linux-gnu"
  exit 0
fi
if [ "$1" = "-dumpversion" ]; then echo "22"; exit 0; fi
if [ "$1" = "-dumpmachine" ]; then echo "x86_64-unknown-linux-gnu"; exit 0; fi
if [ "$1" = "-print-search-dirs" ]; then
  echo "programs: =/usr/lib/llvm-22/bin"
  echo "libraries: =/usr/lib/llvm-22/lib/clang/22/lib"
  exit 0
fi

# Detect C vs C++ and whether this is a link or compile
IS_CXX=0
IS_LINK=0
HAS_C=0
for arg in "$@"; do
  case "$arg" in
    *.cpp|*.cc|*.cxx|*.C|*.c++) IS_CXX=1 ;;
    -shared|-r|-nostdlib|-nostdlib++) IS_LINK=1 ;;
    -c) HAS_C=1 ;;
    -x)
      shift
      case "$1" in
        c++*|objective-c++*) IS_CXX=1 ;;
      esac
      ;;
  esac
done

# If no -c flag is present, this is a link step
if [ "$HAS_C" = "0" ]; then
  IS_LINK=1
fi

if [ "$IS_CXX" = "1" ]; then
  REAL="$CLANGXX"
else
  REAL="$CLANG"
fi

# GL/EGL headers
SKIA_DIR="$ROOT_DIR/build/src/skia"
GL_FLAGS=""
if [ -d "$SKIA_DIR/third_party/externals/opengl-registry/api" ]; then
  GL_FLAGS="-isystem $SKIA_DIR/third_party/externals/opengl-registry/api"
fi
if [ -d "$SKIA_DIR/third_party/externals/egl-registry/api" ]; then
  GL_FLAGS="$GL_FLAGS -isystem $SKIA_DIR/third_party/externals/egl-registry/api"
fi
if [ -d "$SKIA_DIR/third_party/externals/libwebp/src" ]; then
  GL_FLAGS="$GL_FLAGS -isystem $SKIA_DIR/third_party/externals/libwebp/src"
fi
if [ -d "$SKIA_DIR/third_party/externals/libyuv/include" ]; then
  GL_FLAGS="$GL_FLAGS -isystem $SKIA_DIR/third_party/externals/libyuv/include"
fi

# Dawn headers (for Graphite GPU backend via Dawn/OpenGL ES)
DAWN_DIR="$SKIA_DIR/third_party/externals/dawn"
DAWN_GEN="$SKIA_DIR/out/b1nix/gen/third_party/dawn"
if [ -d "$DAWN_DIR/include" ]; then
  GL_FLAGS="$GL_FLAGS -isystem $DAWN_DIR/include"
fi
if [ -d "$DAWN_GEN/include" ]; then
  GL_FLAGS="$GL_FLAGS -isystem $DAWN_GEN/include"
fi

# Fontconfig + freetype + expat
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH_DIR="$ROOT_DIR/build/${B1NIX_ARCH:-x86_64}"
MUSL_SYSROOT="$ARCH_DIR/ports/musl/install"
MUSL_INC="$MUSL_SYSROOT/include"
MUSL_RUNTIME="$ARCH_DIR/toolchain/llvm-runtimes-build-musl"
if [ -d "$MUSL_RUNTIME/libcxx-install/include/c++/v1" ]; then
  LIBCXX_HDR="$MUSL_RUNTIME/libcxx-install/include/c++/v1"
else
  LIBCXX_HDR="$MUSL_SYSROOT/include/c++/v1"
fi
FONTCONFIG_INC="$ARCH_DIR/ports/fontconfig/install/include"
FREETYPE_INC="$SKIA_DIR/third_party/externals/freetype/include"
EXPAT_INC="$SKIA_DIR/third_party/externals/expat/lib"
if [ -d "$FONTCONFIG_INC" ]; then GL_FLAGS="$GL_FLAGS -isystem $FONTCONFIG_INC"; fi
if [ -d "$FREETYPE_INC" ]; then GL_FLAGS="$GL_FLAGS -isystem $FREETYPE_INC"; fi
if [ -d "$EXPAT_INC" ]; then GL_FLAGS="$GL_FLAGS -isystem $EXPAT_INC"; fi

if [ "$IS_LINK" = "1" ]; then
  # Link step: use -fuse-ld=lld to force lld with b1nix sysroot.
  # Filter out host -l flags — b1nix libraries resolve from the sysroot.
  ARGS=()
  SKIP_NEXT=0
  for arg in "$@"; do
    if [ "$SKIP_NEXT" = "1" ]; then
      SKIP_NEXT=0
      ARGS+=("$arg")
      continue
    fi
    ARGS+=("$arg")
  done
  # libc++ runtime (for Dawn/Harfbuzz std::mutex etc.)
  LIBCXX_RT="$MUSL_SYSROOT/lib"
  LIBCXX_L=""
  [ -d "$LIBCXX_RT" ] && LIBCXX_L="-L $LIBCXX_RT"

  # Mesa GL/EGL shared libs (OSMesa softpipe backend for b1nix).
  MESA_LIB_DIR="$ARCH_DIR/ports/mesa/install/lib"
  FONTCONFIG_LIB_DIR="$ARCH_DIR/ports/fontconfig/install/lib"
  FREETYPE_LIB_DIR="$ARCH_DIR/ports/freetype/install/lib"
  EXPAT_LIB_DIR="$ARCH_DIR/ports/expat/install/lib"
  ZLIB_LIB_DIR="$ARCH_DIR/ports/zlib/install/lib"

  exec "$REAL" \
    --target=${B1NIX_TRIPLET:-x86_64-b1nix} \
    --sysroot="$MUSL_SYSROOT" \
    -fuse-ld=lld \
    -Wl,--hash-style=both \
    -nostartfiles \
    -L "$MUSL_SYSROOT/lib" \
    -L "$FONTCONFIG_LIB_DIR" \
    -L "$FREETYPE_LIB_DIR" \
    -L "$EXPAT_LIB_DIR" \
    -L "$ZLIB_LIB_DIR" \
    -L "$MESA_LIB_DIR" \
    -L "$ROOT_DIR/userspace/build/${B1NIX_ARCH:-x86_64}" \
    $LIBCXX_L \
    -D__b1nix__ -D__linux__ -Db1nix \
    -nostdinc++ \
    -nostdinc \
    -isystem "$LIBCXX_HDR" \
    -isystem "$LIBCXX_HDR/x86_64-unknown-linux-gnu" \
    -isystem "$(clang -print-resource-dir)/include" \
    -isystem "$MUSL_INC" \
    -idirafter "$CROSS/x86_64-b1nix/include" \
    $GL_FLAGS \
    "${ARGS[@]}" \
    -Wl,--start-group \
    $PLATFORM_LIBS \
    -Wl,-Bstatic -lfontconfig -lfreetype -lexpat -lz -lm -Wl,-Bdynamic \
    -lc++ -lc++abi -lunwind \
    $MESA_LIBS \
    -Wl,--end-group
else
  # Compile step: add include paths
  exec "$REAL" \
    --target=x86_64-unknown-linux-gnu \
    --sysroot="$CROSS" \
    -D__b1nix__ -D__linux__ -Db1nix \
    $GL_FLAGS \
    -nostdinc++ \
    -nostdinc \
    -isystem "$LIBCXX_HDR" \
    -isystem "$LIBCXX_HDR/x86_64-unknown-linux-gnu" \
    -isystem "$(clang -print-resource-dir)/include" \
    -isystem "$MUSL_INC" \
    -idirafter "$CROSS/x86_64-b1nix/include" \
    "$@"
fi
