#!/bin/sh
# Apply the b1nix GN-target skeleton patches to a V8 checkout.
# See tools/patches/v8/PORT-PLAN.md for the rationale behind each one.
#
#   sh tools/patches/v8/apply.sh <path-to-v8-checkout>
#
# Idempotent: every patch is grep-guarded, so re-running is a no-op, and each
# verifies its own marker afterwards (fails loud if upstream drifted and the
# anchor no longer matches — re-grep PORT-PLAN's anchors then).
#
# IMPORTANT: Patches 1 & 2 live in //build (the shared Chromium build module),
# which `gclient sync` re-pulls at its DEPS-pinned revision — so they MUST be
# re-applied after every sync. tools/sync-v8.sh calls this for exactly that.
set -eu

V8="${1:?usage: apply.sh <path-to-v8-checkout>}"
PATCHDIR="$(cd "$(dirname "$0")" && pwd)"
[ -f "$V8/include/v8config.h" ] || { echo "not a v8 checkout: $V8"; exit 1; }
BUILD="$V8/build"
[ -d "$BUILD/config" ] || { echo "//build missing (run gclient sync first): $BUILD"; exit 1; }

die() { echo "apply.sh: $1" >&2; exit 1; }

# --- Patch 1: //build/config/BUILDCONFIG.gn default-toolchain dispatch ---------
F="$BUILD/config/BUILDCONFIG.gn"
if ! grep -q '//build/toolchain/b1nix:' "$F"; then
  perl -0777 -i -pe 's~(\} else if \(target_os == "zos"\) \{\n  _default_toolchain = "//build/toolchain/zos:\$target_cpu"\n)~${1}} else if (target_os == "b1nix") {\n  _default_toolchain = "//build/toolchain/b1nix:\$target_cpu"\n~' "$F"
  grep -q '//build/toolchain/b1nix:' "$F" || die "Patch 1 anchor not found in $F"
  echo "Patch 1 applied: BUILDCONFIG.gn"
else echo "Patch 1 already present"; fi

# --- Patch 2: //build/toolchain/b1nix/BUILD.gn (net-new) -----------------------
mkdir -p "$BUILD/toolchain/b1nix"
cp -f "$PATCHDIR/toolchain/b1nix/BUILD.gn" "$BUILD/toolchain/b1nix/BUILD.gn"
echo "Patch 2 applied: toolchain/b1nix/BUILD.gn"

# --- Patch 3: v8/include/v8config.h OS detection ------------------------------
F="$V8/include/v8config.h"
if ! grep -q 'defined(__b1nix__)' "$F"; then
  perl -0777 -i -pe 's~(\n)(#elif defined\(__sun\))~${1}#elif defined(__b1nix__)\n# define V8_OS_LINUX 1\n# define V8_OS_POSIX 1\n# define V8_OS_STRING "b1nix"\n${1}${2}~' "$F"
  grep -q 'defined(__b1nix__)' "$F" || die "Patch 3 anchor not found in $F"
  echo "Patch 3 applied: v8config.h"
else echo "Patch 3 already present"; fi

# --- Patch 4a: v8/BUILD.gn V8_TARGET_OS defines -------------------------------
F="$V8/BUILD.gn"
if ! grep -q 'target_os == "b1nix"' "$F"; then
  perl -0777 -i -pe 's~(  enabled_external_v8_defines \+= \[ "V8_TARGET_OS_CHROMEOS" \]\n)\}~${1}} else if (target_os == "b1nix") {\n  enabled_external_v8_defines += [ "V8_HAVE_TARGET_OS" ]\n  enabled_external_v8_defines += [ "V8_TARGET_OS_LINUX" ]\n}~' "$F"
  grep -q 'target_os == "b1nix"' "$F" || die "Patch 4a anchor not found in $F"
  echo "Patch 4a applied: BUILD.gn (V8_TARGET_OS)"
else echo "Patch 4a already present"; fi

# --- Patch 4b: v8/BUILD.gn v8_libbase platform source selection ---------------
if ! grep -q 'current_os == "b1nix"' "$F"; then
  perl -0777 -i -pe 's~(    libs = \[\n      "dl",\n      "rt",\n    \]\n)(  \} else if \(current_os == "aix"\) \{)~${1}  } else if (current_os == "b1nix") {\n    sources += [\n      "src/base/debug/stack_trace_posix.cc",\n      "src/base/platform/platform-linux.cc",\n      "src/base/platform/platform-linux.h",\n    ]\n\n    # b1nix has no -ldl/-lrt (static libc) — link nothing extra.\n${2}~' "$F"
  grep -q 'current_os == "b1nix"' "$F" || die "Patch 4b anchor not found in $F"
  echo "Patch 4b applied: BUILD.gn (platform sources)"
else echo "Patch 4b already present"; fi

# --- Patch 5: platform-linux.cc — guard the <sys/prctl.h> include -------------
# b1nix has no <sys/prctl.h> and platform-linux.cc never calls prctl() — it only
# includes the header. Guard the include for b1nix. (PORT-PLAN appendix A.)
F="$V8/src/base/platform/platform-linux.cc"
[ -f "$F" ] || die "platform-linux.cc not found at $F"
if ! grep -q 'no <sys/prctl.h>' "$F"; then
  perl -0777 -i -pe 's~^#include <sys/prctl\.h>$~#if !defined(__b1nix__)  // b1nix: no <sys/prctl.h> (prctl never called here)\n#include <sys/prctl.h>\n#endif~m' "$F"
  grep -q 'no <sys/prctl.h>' "$F" || die "Patch 5 anchor not found in $F"
  echo "Patch 5 applied: platform-linux.cc (prctl include guard)"
else echo "Patch 5 already present"; fi

# --- Patch 6: platform-linux.cc — stub OS::RemapShared (no mremap on b1nix) ----
# RemapShared is the pointer-compression shared-cage remap path, unreachable
# under --jitless; b1nix has no mremap syscall. Stub to nullptr. (appendix A.)
if ! grep -q 'b1nix has no mremap' "$F"; then
  perl -0777 -i -pe 's~(void\* OS::RemapShared\(void\* old_address, void\* new_address, size_t size\) \{\n)~${1}#if defined(__b1nix__)\n  // b1nix has no mremap (shared-cage remap, dead under --jitless); stub it.\n  (void)old_address; (void)new_address; (void)size;\n  return nullptr;\n#else\n~' "$F"
  perl -0777 -i -pe 's~(  DCHECK\(result == new_address\);\n  return result;\n)\}~${1}#endif\n}~' "$F"
  grep -q 'b1nix has no mremap' "$F" || die "Patch 6 anchor not found in $F"
  echo "Patch 6 applied: platform-linux.cc (RemapShared stub)"
else echo "Patch 6 already present"; fi

# --- Patch 7: //build/config/rust.gni — give b1nix a rust_abi_target ----------
# The `coverage` default-config imports rust.gni for EVERY target, tripping
# `assert(rust_abi_target != "")`. Alias b1nix to the linux triple. No Rust is
# actually built (enable_rust off, temporal off) — this only clears the assert.
F="$BUILD/config/rust.gni"
if ! grep -q 'current_os == "b1nix"' "$F"; then
  perl -0777 -i -pe 's~(rust_abi_target = ""\nif \(is_linux \|\| is_chromeos)(\) \{)~${1} || current_os == "b1nix"${2}~' "$F"
  grep -q 'current_os == "b1nix"' "$F" || die "Patch 7 anchor not found in $F"
  echo "Patch 7 applied: rust.gni (b1nix rust_abi_target)"
else echo "Patch 7 already present"; fi

# --- Patch 8: //build/config/clang/BUILD.gn — clang_rt dir for b1nix ----------
# clang_lib()'s platform dispatch runs at gen time (not guarded by is_clang) and
# asserts on unknown OS. Alias b1nix to the linux clang_rt path. We build with
# GCC so the lib is never linked; this only clears the gen-time assert.
F="$BUILD/config/clang/BUILD.gn"
if ! grep -q 'current_os == "b1nix"' "$F"; then
  perl -0777 -i -pe 's~(_dir = "darwin"\n      \} else if \(is_linux \|\| is_chromeos)(\) \{)~${1} || current_os == "b1nix"${2}~' "$F"
  grep -q 'current_os == "b1nix"' "$F" || die "Patch 8 anchor not found in $F"
  echo "Patch 8 applied: clang/BUILD.gn (b1nix clang_rt dir)"
else echo "Patch 8 already present"; fi

# ============================================================================
# v8_libbase compile chase (Patches 9-13). Found by building `ninja v8_libbase`
# with the x86_64-b1nix cross GCC. 9-11 are in v8-proper (survive sync);
# 12-13 are in third_party/abseil-cpp (re-pulled by sync — MUST re-apply).
# ============================================================================

# --- Patch 9: src/base/macros.h — __has_warning fallback for GCC -------------
# Clang-only builtin; GCC fails to even parse the defined()-guarded uses below.
F="$V8/src/base/macros.h"
if ! grep -q '!defined(__has_warning)' "$F"; then
  perl -0777 -i -pe 's~(#ifndef V8_BASE_MACROS_H_\n#define V8_BASE_MACROS_H_\n)~${1}\n#if !defined(__has_warning)  /* b1nix/GCC: clang-only builtin */\n#define __has_warning(x) 0\n#endif\n~' "$F"
  grep -q '!defined(__has_warning)' "$F" || die "Patch 9 anchor not found in $F"
  echo "Patch 9 applied: macros.h (__has_warning fallback)"
else echo "Patch 9 already present"; fi

# --- Patch 10: cpu.cc + cpu-x86.cc — drop Linux auxv includes on b1nix -------
# <linux/auxvec.h>/<sys/auxv.h> are only used for ARM-family HWCAP detection;
# b1nix is x64 and lacks those headers.
for F in "$V8/src/base/cpu/cpu.cc" "$V8/src/base/cpu/cpu-x86.cc"; do
  if ! grep -q 'V8_OS_LINUX && !defined(__b1nix__)' "$F"; then
    perl -0777 -i -pe 's~#if V8_OS_LINUX\n#include <linux/auxvec\.h>~#if V8_OS_LINUX && !defined(__b1nix__)\n#include <linux/auxvec.h>~g; s~#if V8_OS_LINUX\n#include <sys/auxv\.h>~#if V8_OS_LINUX && !defined(__b1nix__)\n#include <sys/auxv.h>~g' "$F"
    grep -q 'V8_OS_LINUX && !defined(__b1nix__)' "$F" || die "Patch 10 anchor not found in $F"
    echo "Patch 10 applied: $(basename "$F") (auxv include guard)"
  else echo "Patch 10 already present: $(basename "$F")"; fi
done

# --- Patch 11: src/base/build_config.h — disable PKU on b1nix ----------------
# PKU = JIT W^X via protection keys; pointless under --jitless, and it pulls
# glibc-only pthread_getattr_np/PROT_GROWSDOWN.
F="$V8/src/base/build_config.h"
if ! grep -q 'jitless: no JIT W' "$F"; then
  perl -0777 -i -pe 's~(#if defined\(V8_OS_LINUX\) && defined\(V8_HOST_ARCH_X64\))(\n#define V8_HAS_PKU_SUPPORT 1)~${1} && !defined(__b1nix__)  /* jitless: no JIT W^X */${2}~' "$F"
  grep -q 'jitless: no JIT W' "$F" || die "Patch 11 anchor not found in $F"
  echo "Patch 11 applied: build_config.h (PKU off)"
else echo "Patch 11 already present"; fi

# --- Patch 12: abseil elf_mem_image.h — no ELF symbolizer on b1nix ----------
# Avoids <link.h>/dl_iterate_phdr; absl symbolization isn't needed for jitless.
F="$V8/third_party/abseil-cpp/absl/debugging/internal/elf_mem_image.h"
if [ -f "$F" ] && ! grep -q '!defined(__b1nix__)' "$F"; then
  perl -0777 -i -pe 's~(!defined\(__XTENSA__\))\n(#define ABSL_HAVE_ELF_MEM_IMAGE 1)~${1} && \\\n    !defined(__b1nix__)\n${2}~' "$F"
  grep -q '!defined(__b1nix__)' "$F" || die "Patch 12 anchor not found in $F"
  echo "Patch 12 applied: abseil elf_mem_image.h (no ELF symbolizer)"
else echo "Patch 12 already present/absent"; fi

# --- Patch 13: abseil str_format/arg.cc — ::wcslen (no std::wcslen) ----------
# b1nix libstdc++ was built without _GLIBCXX_USE_WCHAR_T, so <cwchar> doesn't
# export std::wcslen; b1nix libc provides the C wcslen.
F="$V8/third_party/abseil-cpp/absl/strings/internal/str_format/arg.cc"
if [ -f "$F" ] && ! grep -q '::wcslen' "$F"; then
  perl -0777 -i -pe 's~#include <cwchar>~#include <cwchar>\n#include <wchar.h>  // b1nix: libstdc++ lacks _GLIBCXX_USE_WCHAR_T~; s~std::wcslen~::wcslen~g' "$F"
  grep -q '::wcslen' "$F" || die "Patch 13 anchor not found in $F"
  echo "Patch 13 applied: abseil arg.cc (::wcslen)"
else echo "Patch 13 already present/absent"; fi

# --- Patch 14: //build/build_config.h — b1nix is OS_LINUX/IS_POSIX -----------
# Chromium's build_config.h (partition_alloc, cppgc, ...) keys OS detection on
# __linux__, which b1nix doesn't define. Without this b1nix isn't IS_POSIX and
# partition_alloc's per-OS types (ScopedClearLastError, ProcessId, posix
# sources) don't resolve. Alias b1nix to OS_LINUX. In //build → wiped by sync.
F="$BUILD/build_config.h"
if ! grep -q 'defined(__b1nix__)' "$F"; then
  perl -0777 -i -pe 's~(#elif defined\(_WIN32\)\n#define OS_WIN 1)~#elif defined(__b1nix__)\n#define OS_LINUX 1\n${1}~' "$F"
  grep -q 'defined(__b1nix__)' "$F" || die "Patch 14 anchor not found in $F"
  echo "Patch 14 applied: build_config.h (b1nix OS_LINUX)"
else echo "Patch 14 already present"; fi

# --- Patch 15: partition_alloc/build_config.h — b1nix is PA_IS_LINUX ---------
# partition_alloc has its OWN build_config.h fork (PA_BUILDFLAG(IS_POSIX) etc.),
# also keyed on __linux__. third_party → wiped by sync.
F="$V8/third_party/partition_alloc/src/partition_alloc/build_config.h"
if [ -f "$F" ] && ! grep -q 'defined(__b1nix__)' "$F"; then
  perl -0777 -i -pe 's~(#elif defined\(_WIN32\)\n#define PA_IS_WIN)~#elif defined(__b1nix__)\n#define PA_IS_LINUX\n${1}~' "$F"
  grep -q 'defined(__b1nix__)' "$F" || die "Patch 15 anchor not found in $F"
  echo "Patch 15 applied: partition_alloc build_config.h (PA_IS_LINUX)"
else echo "Patch 15 already present/absent"; fi

# --- Patch 16: //build/config/compiler/BUILD.gn — -std=gnu++20 for GCC --------
# V8 sets -std=c++20; in strict c++20 mode GCC DISABLES the GNU ", ##__VA_ARGS__"
# comma-swallowing extension that V8's descriptor macros rely on, producing
# hundreds of "expected identifier before ',' token" errors. clang keeps it.
# Use gnu++20 for the GCC (b1nix) target; leave clang (host) on c++20. //build.
F="$BUILD/config/compiler/BUILD.gn"
if ! grep -q 'gnu++20' "$F"; then
  perl -0777 -i -pe 's~    \} else \{\n      cflags_cc \+= \[ "-std=c\+\+20" \]\n    \}~    } else if (is_clang) {\n      cflags_cc += [ "-std=c++20" ]\n    } else {\n      cflags_cc += [ "-std=gnu++20" ]  # b1nix/GCC: keep GNU ,##__VA_ARGS__\n    }~' "$F"
  grep -q 'gnu++20' "$F" || die "Patch 16 anchor not found in $F"
  echo "Patch 16 applied: compiler/BUILD.gn (gnu++20 for GCC)"
else echo "Patch 16 already present"; fi

echo "All b1nix V8 patches applied to $V8"
