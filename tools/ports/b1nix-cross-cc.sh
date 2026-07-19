#!/bin/sh
# b1nix cross-compiler wrapper for Skia GN builds.
# Calls host clang/clang++ with --target + --sysroot for cross-compilation.
# Handles compilation AND linking: when -shared or -o linking is detected,
# uses ld.lld with b1nix sysroot instead of host linker.

CROSS="/home/dmytrom/Documents/GitHub/b1nix/build/toolchain_build/x86_64-b1nix/cross"
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
SKIA_DIR="/home/dmytrom/Documents/GitHub/b1nix/build/ports-src/skia"
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
MUSL_INC="$ROOT_DIR/build/musl-b1nix/x86_64-b1nix/install/usr/include"
MUSL_RUNTIME="$ROOT_DIR/build/toolchain_build/x86_64-b1nix/llvm-runtimes-build-musl"
if [ -d "$MUSL_RUNTIME/libcxx-install/include/c++/v1" ]; then
  LIBCXX_HDR="$MUSL_RUNTIME/libcxx-install/include/c++/v1"
else
  LIBCXX_HDR="$CROSS/x86_64-b1nix/include/c++/v1"
fi
FONTCONFIG_INC="$ROOT_DIR/build/fontconfig-b1nix/x86_64-b1nix/install/include"
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
  LIBCXX_RT="$ROOT_DIR/build/toolchain_build/x86_64-b1nix/llvm-runtimes-build/libcxx-install/lib"
  LIBCXX_L=""
  [ -d "$LIBCXX_RT" ] && LIBCXX_L="-L $LIBCXX_RT"

  # Mesa GL/EGL shared libs (OSMesa softpipe backend for b1nix).
  # libOSMesa.so is the main consumer-facing lib; it DT_NEEDS libgallium.so,
  # libglapi.so, etc. which the dynamic linker resolves at runtime.
  MESA_LIB_DIR="$ROOT_DIR/build/mesa-b1nix/x86_64-b1nix/install/lib"
  MESA_L=""
  MESA_LIBS=""
  if [ -d "$MESA_LIB_DIR" ]; then
    MESA_L="-L $MESA_LIB_DIR"
    MESA_LIBS="-lOSMesa"
  fi

  # b1nix platform libs: crt0 (startup), EGL, c11threads, b1nix libc, b1nix GUI
  SYSROOT_LIB="$CROSS/x86_64-b1nix/lib"
  PLATFORM_LIBS=""
  # b1nix CRT startup (replaces host's crt1/Scrt1.o)
  [ -f "$SYSROOT_LIB/crt0.o" ] && \
    PLATFORM_LIBS="$PLATFORM_LIBS $SYSROOT_LIB/crt0.o"
  # EGL implementation (wraps OSMesa, provides eglCreateContext etc.)
  [ -f "$SYSROOT_LIB/libEGL_b1nix.a" ] && \
    PLATFORM_LIBS="$PLATFORM_LIBS $SYSROOT_LIB/libEGL_b1nix.a"
  [ -f "$SYSROOT_LIB/libb1nix_c11threads.a" ] && \
    PLATFORM_LIBS="$PLATFORM_LIBS -Wl,--whole-archive $SYSROOT_LIB/libb1nix_c11threads.a -Wl,--no-whole-archive"
  [ -f "$ROOT_DIR/userspace/build/x86_64/libb1nix.a" ] && \
    PLATFORM_LIBS="$PLATFORM_LIBS $ROOT_DIR/userspace/build/x86_64/libb1nix.a"
  [ -f "$ROOT_DIR/userspace/build/x86_64/libb1gui.a" ] && \
    PLATFORM_LIBS="$PLATFORM_LIBS $ROOT_DIR/userspace/build/x86_64/libb1gui.a"

  # b1nix's in-kernel eager dynamic linker resolves symbols through the SysV
  # DT_HASH table only; lld defaults to .gnu.hash for linux targets, so force
  # both so libskia.so/libraw_ptr.so carry the SysV hash the loader needs.
  exec "$REAL" \
    --target=x86_64-unknown-linux-gnu \
    --sysroot="$CROSS" \
    -fuse-ld=lld \
    -Wl,--hash-style=both \
    -Wl,-dynamic-linker=/lib/ld-b1nix.so \
    -nostartfiles \
    -rtlib=compiler-rt -unwindlib=none -static-libgcc \
    -L "$CROSS/x86_64-b1nix/lib" \
    -L "$CROSS/lib" \
    -L "$ROOT_DIR/userspace/build/x86_64" \
    $LIBCXX_L \
    $MESA_L \
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
