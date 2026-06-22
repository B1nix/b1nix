#!/bin/sh
# Apply the b1nix //build patches to a CHROMIUM checkout (M61).
#
#   sh tools/patches/chromium/apply.sh <path-to-chromium-src>
#
# Scope: only the shared //build module patches needed for `gn gen` to accept
# target_os=b1nix and for base/ to detect b1nix as OS_LINUX/POSIX. These mirror
# the //build group of tools/patches/v8/apply.sh (same anchors, same toolchain
# file) but for the Chromium layout (//build = src/build). The v8-proper /
# abseil / partition_alloc compile patches are applied as `ninja` hits them —
# that error-chase is the M61 work, tracked in tools/patches/chromium/PORT-PLAN.md.
#
# Idempotent + grep-guarded; re-run after every `gclient sync` (sync re-pulls
# //build at its DEPS-pinned revision).
set -eu

SRC="${1:?usage: apply.sh <path-to-chromium-src>}"
V8PATCH="$(cd "$(dirname "$0")/../v8" && pwd)"   # reuse the toolchain/b1nix file
[ -f "$SRC/v8/include/v8config.h" ] || { echo "not a chromium checkout: $SRC"; exit 1; }
BUILD="$SRC/build"
[ -d "$BUILD/config" ] || { echo "//build missing (run gclient sync first): $BUILD"; exit 1; }

die() { echo "apply.sh: $1" >&2; exit 1; }

# --- Patch 1: BUILDCONFIG.gn default-toolchain dispatch -----------------------
F="$BUILD/config/BUILDCONFIG.gn"
if ! grep -q '//build/toolchain/b1nix:' "$F"; then
  perl -0777 -i -pe 's~(\} else if \(target_os == "zos"\) \{\n  _default_toolchain = "//build/toolchain/zos:\$target_cpu"\n)~${1}} else if (target_os == "b1nix") {\n  _default_toolchain = "//build/toolchain/b1nix:\$target_cpu"\n~' "$F"
  grep -q '//build/toolchain/b1nix:' "$F" || die "Patch 1 anchor not found in $F"
  echo "Patch 1 applied: BUILDCONFIG.gn"
else echo "Patch 1 already present"; fi

# --- Patch 2: //build/toolchain/b1nix/BUILD.gn (net-new) ---------------------
mkdir -p "$BUILD/toolchain/b1nix"
cp -f "$V8PATCH/toolchain/b1nix/BUILD.gn" "$BUILD/toolchain/b1nix/BUILD.gn"
echo "Patch 2 applied: toolchain/b1nix/BUILD.gn"

# --- Patch 7: rust.gni — give b1nix a rust_abi_target ------------------------
F="$BUILD/config/rust.gni"
if ! grep -q 'current_os == "b1nix"' "$F"; then
  perl -0777 -i -pe 's~(rust_abi_target = ""\nif \(is_linux \|\| is_chromeos)(\) \{)~${1} || current_os == "b1nix"${2}~' "$F"
  grep -q 'current_os == "b1nix"' "$F" || die "Patch 7 anchor not found in $F"
  echo "Patch 7 applied: rust.gni"
else echo "Patch 7 already present"; fi

# --- Patch 8: clang/BUILD.gn — clang_rt dir for b1nix ------------------------
F="$BUILD/config/clang/BUILD.gn"
if ! grep -q 'current_os == "b1nix"' "$F"; then
  perl -0777 -i -pe 's~(_dir = "darwin"\n      \} else if \(is_linux \|\| is_chromeos)(\) \{)~${1} || current_os == "b1nix"${2}~' "$F"
  grep -q 'current_os == "b1nix"' "$F" || die "Patch 8 anchor not found in $F"
  echo "Patch 8 applied: clang/BUILD.gn"
else echo "Patch 8 already present"; fi

# --- Patch 14: //build/build_config.h — b1nix is OS_LINUX/IS_POSIX -----------
F="$BUILD/build_config.h"
if ! grep -q 'defined(__b1nix__)' "$F"; then
  perl -0777 -i -pe 's~(#elif defined\(_WIN32\)\n#define OS_WIN 1)~#elif defined(__b1nix__)\n#define OS_LINUX 1\n${1}~' "$F"
  grep -q 'defined(__b1nix__)' "$F" || die "Patch 14 anchor not found in $F"
  echo "Patch 14 applied: build_config.h (OS_LINUX)"
else echo "Patch 14 already present"; fi

# --- Patch 16: compiler/BUILD.gn — -std=gnu++20 for GCC ----------------------
F="$BUILD/config/compiler/BUILD.gn"
if ! grep -q 'gnu++20' "$F"; then
  perl -0777 -i -pe 's~    \} else \{\n      cflags_cc \+= \[ "-std=c\+\+20" \]\n    \}~    } else if (is_clang) {\n      cflags_cc += [ "-std=c++20" ]\n    } else {\n      cflags_cc += [ "-std=gnu++20" ]  # b1nix/GCC: keep GNU ,##__VA_ARGS__\n    }~' "$F"
  grep -q 'gnu++20' "$F" || die "Patch 16 anchor not found in $F"
  echo "Patch 16 applied: compiler/BUILD.gn (gnu++20)"
else echo "Patch 16 already present"; fi

# --- Patch C1: BUILDCONFIG.gn — b1nix counts as is_linux (GN level) ----------
# At the GN level is_linux = (current_os=="linux"). For target_os=b1nix it would
# be false everywhere, skipping thousands of is_linux-guarded sources/data/deps
# (e.g. content/test's `data +=` on an uninitialized var). Aliasing b1nix to
# is_linux inherits Chromium's whole desktop-linux GN logic — the high-leverage
# inversion of the V8 own-OS approach. b1nix is already is_posix (=!win&&!fuchsia).
F="$BUILD/config/BUILDCONFIG.gn"
if ! grep -q 'current_os == "linux" || current_os == "b1nix"' "$F"; then
  perl -i -pe 's~^is_linux = current_os == "linux"$~is_linux = current_os == "linux" || current_os == "b1nix"  # b1nix port (M61): inherit desktop-linux GN logic~' "$F"
  grep -q 'current_os == "b1nix"' "$F" || die "Patch C1 anchor not found in $F"
  echo "Patch C1 applied: BUILDCONFIG.gn (is_linux alias)"
else echo "Patch C1 already present"; fi

# --- Patch C2: //BUILD.gn — minimal gn_all for b1nix -------------------------
# gn_all otherwise drags in the whole chrome + test universe. Scope b1nix's
# gn_all to content_shell (the M62 target). NOTE: other top-level groups in
# //BUILD.gn still evaluate chrome/test under is_linux — scoping those is the
# next open step (see PORT-PLAN.md).
F="$SRC/BUILD.gn"
if ! grep -q 'target_os == "b1nix"' "$F"; then
  perl -0777 -i -pe 's~(group\("gn_all"\) \{\n  testonly = true\n\n)(  if \(is_cronet_build\) \{)~${1}  if (target_os == "b1nix") {\n    # b1nix port (M61): only content_shell + its deps, not the chrome/test universe.\n    deps = [ "//content/shell:content_shell" ]\n  } else if (is_cronet_build) {~' "$F"
  grep -q 'target_os == "b1nix"' "$F" || die "Patch C2 anchor not found in $F"
  echo "Patch C2 applied: //BUILD.gn (minimal gn_all)"
else echo "Patch C2 already present"; fi

# --- Patch C3: //BUILD.gn — scope ALL aggregate root groups for b1nix --------
# gn gen evaluates EVERY top-level construct in //BUILD.gn (the `all` meta-target),
# not just gn_all. With is_linux=true (C1) the post-gn_all groups/if-blocks
# (all_rust, rust_build_tests, chromium_builder_perf, web-test + chrome if-blocks)
# drag in //chrome and //chrome/test. Wrap that whole region (after gn_all, before
# the final gn_logs bookkeeping) in `if (target_os != "b1nix")`.
F="$SRC/BUILD.gn"
if ! grep -q 'b1nix port: skip aggregate' "$F"; then
  perl -0777 -i -pe 's~\n(# All Rust targets\. This is provided for convenience)~\nif (target_os != "b1nix") {  # b1nix port: skip aggregate/test/rust/chrome root groups\n\n${1}~' "$F"
  perl -0777 -i -pe 's~\n(# GN evaluates each \.gn file once per toolchain)~\n}  # end b1nix root-scope guard\n\n${1}~' "$F"
  { grep -q 'b1nix port: skip aggregate' "$F" && grep -q 'end b1nix root-scope guard' "$F"; } || die "Patch C3 anchors not found in $F"
  echo "Patch C3 applied: //BUILD.gn (root-scope aggregate groups)"
else echo "Patch C3 already present"; fi

# --- Patch C4: content/shell — drop GTK/X11 linux_ui_factory for b1nix -------
# content_shell's `if (is_linux)` pulls //ui/linux:linux_ui_factory (the GTK/X11
# desktop-UI stack). b1nix is headless (ozone_platform_headless) and has no GTK,
# so cut that edge. Correct on its own merits (headless build).
F="$SRC/content/shell/BUILD.gn"
if ! grep -q 'b1nix: headless, no GTK' "$F"; then
  perl -0777 -i -pe 's~  if \(is_linux\) \{\n    deps \+= \[ "//ui/linux:linux_ui_factory" \]\n  \}~  if (is_linux \&\& target_os != "b1nix") {  # b1nix: headless, no GTK/X11 desktop UI\n    deps += [ "//ui/linux:linux_ui_factory" ]\n  }~' "$F"
  grep -q 'b1nix: headless, no GTK' "$F" || die "Patch C4 anchor not found in $F"
  echo "Patch C4 applied: content/shell (drop linux_ui_factory)"
else echo "Patch C4 already present"; fi

# --- Patch C5: content/shell — drop chrome_crashpad_handler for b1nix --------
# content_shell's linux branch adds //components/crash/core/app:chrome_crashpad_handler
# as a data_dep. That handler is the chrome-flavoured crashpad and is unnecessary
# for the headless M62 bring-up (no crash reporting yet). Cut it for b1nix.
F="$SRC/content/shell/BUILD.gn"
if ! grep -q 'b1nix: no chrome crashpad' "$F"; then
  perl -0777 -i -pe 's~  \} else if \(is_linux \|\| is_chromeos\) \{\n      data_deps \+= \[ "//components/crash/core/app:chrome_crashpad_handler" \]\n    \}~  } else if ((is_linux || is_chromeos) \&\& target_os != "b1nix") {  # b1nix: no chrome crashpad handler\n      data_deps += [ "//components/crash/core/app:chrome_crashpad_handler" ]\n    }~' "$F"
  grep -q 'b1nix: no chrome crashpad' "$F" || die "Patch C5 anchor not found in $F"
  echo "Patch C5 applied: content/shell (drop chrome_crashpad_handler)"
else echo "Patch C5 already present"; fi

# --- Patch C6: enable_webui_ntp — treat b1nix as a desktop platform ----------
# THE gn-gen unblocker. `gn gen` evaluates every BUILD.gn in the repo (defining,
# but not building, all targets). chrome/browser/ui/BUILD.gn fails its internal
# `allow_circular_includes_from` validation when enable_webui_ntp is false,
# because the NTP circular-include labels are added unconditionally while their
# matching deps sit under `if (enable_webui_ntp)`. enable_webui_ntp is gated on a
# LITERAL `target_os == "linux"` (not is_linux), so b1nix turned it off and broke
# chrome's own BUILD.gn. b1nix is a desktop platform here — enable the WebUI NTP
# like linux/mac/win so chrome's GN stays self-consistent. Nothing extra is
# *compiled* for content_shell (chrome targets are defined, never built by the
# `ninja content_shell` target). Real fix, not an assert bypass.
F="$SRC/ui/webui/webui_features.gni"
if ! grep -q 'b1nix port (M60-62): desktop platform' "$F"; then
  perl -0777 -i -pe 's~(    enable_webui_ntp =\n        target_os == "win" \|\| target_os == "mac" \|\| target_os == "linux" \|\|\n        target_os == "chromeos" \|\| is_desktop_android)~$1 ||\n        target_os == "b1nix"  # b1nix port (M60-62): desktop platform, keep chrome BUILD.gn self-consistent~' "$F"
  grep -q 'b1nix port (M60-62): desktop platform' "$F" || die "Patch C6 anchor not found in $F"
  echo "Patch C6 applied: webui_features.gni (enable_webui_ntp for b1nix)"
else echo "Patch C6 already present"; fi

# --- Patch C7: ffmpeg os_config — reuse the linux x64 asm config for b1nix ----
# third_party/ffmpeg picks a pre-generated per-OS asm config dir via
# `os_config = current_os`; there is no `b1nix` dir. b1nix is x64 with the linux
# ABI, so reuse chromium/config/$branding/linux/x64 (same as chromeos does).
F="$SRC/third_party/ffmpeg/ffmpeg_options.gni"
if ! grep -q 'b1nix port: reuse the linux x64 ffmpeg' "$F"; then
  perl -0777 -i -pe 's~(os_config = current_os\n)(if \(\(is_linux \|\| is_chromeos\) \&\& is_msan\) \{)~${1}if (current_os == "b1nix") {\n  os_config = "linux"  # b1nix port: reuse the linux x64 ffmpeg asm config (same ABI)\n} else if ((is_linux || is_chromeos) \&\& is_msan) {~' "$F"
  grep -q 'b1nix port: reuse the linux x64 ffmpeg' "$F" || die "Patch C7 anchor not found in $F"
  echo "Patch C7 applied: ffmpeg_options.gni (os_config=linux for b1nix)"
else echo "Patch C7 already present"; fi

# --- Patch C8: BUILDCONFIG.gn — is_clang=false for b1nix (GCC cross build) ----
# THE compile unblocker. b1nix's default toolchain wraps the x86_64-b1nix cross
# GCC, but `is_clang`'s declare_args() default is `current_os != "linux" || ...`,
# which is TRUE for b1nix → the whole build emits clang-only flags (-Xclang,
# -mllvm, --target=, -Wgnu, -fcolor-diagnostics, ...) that GCC rejects. The
# toolchain's own `toolchain_args { is_clang = false }` is IGNORED because b1nix
# is the *default* toolchain (gn ignores toolchain_args for the default tc). So
# the value must come from the global default: force is_clang=false for b1nix
# here. linux/mac/win keep their original value; the host clang_x64 toolchain
# sets is_clang in its own (non-default) toolchain_args, so the host stays clang.
F="$BUILD/config/BUILDCONFIG.gn"
if ! grep -q 'b1nix port: build with the GCC cross toolchain' "$F"; then
  perl -0777 -i -pe 's~(  # Set to true when compiling with the Clang compiler\.\n)(  is_clang = current_os != "linux" \|\|\n             \(current_cpu != "mips" && current_cpu != "mips64"\))~${1}  is_clang = current_os != "b1nix" \&\&  # b1nix port: build with the GCC cross toolchain\n             (current_os != "linux" ||\n             (current_cpu != "mips" \&\& current_cpu != "mips64"))~' "$F"
  grep -q 'b1nix port: build with the GCC cross toolchain' "$F" || die "Patch C8 anchor not found in $F"
  echo "Patch C8 applied: BUILDCONFIG.gn (is_clang=false for b1nix)"
else echo "Patch C8 already present"; fi

# --- Patch C9: compiler/BUILD.gn — no -pthread cflag for b1nix ---------------
# The b1nix cross GCC has no `-pthread` driver flag (pthread is folded into
# libb1nix.a/libc.a, not a separate runtime/spec). Chromium adds `-pthread` to
# cflags under `if (is_linux || is_chromeos)` (true for b1nix via C1), which the
# cross GCC rejects ("unrecognized command-line option '-pthread'"). Skip it.
F="$BUILD/config/compiler/BUILD.gn"
if ! grep -q 'b1nix: pthread lives in libb1nix.a' "$F"; then
  perl -0777 -i -pe 's~(  if \(is_linux \|\| is_chromeos\) \{\n)    cflags \+= \[ "-pthread" \]~${1}    if (target_os != "b1nix") {  # b1nix: pthread lives in libb1nix.a; GCC has no -pthread\n      cflags += [ "-pthread" ]\n    }~' "$F"
  grep -q 'b1nix: pthread lives in libb1nix.a' "$F" || die "Patch C9 anchor not found in $F"
  echo "Patch C9 applied: compiler/BUILD.gn (no -pthread for b1nix)"
else echo "Patch C9 already present"; fi

# --- Patch C10: //build/config:default_libs — empty for b1nix ----------------
# The is_linux default_libs link dl/pthread/rt, none of which exist as standalone
# libs in the b1nix sysroot (folded into libc.a/libb1nix.a, linked implicitly by
# the driver). Give b1nix an empty default_libs. MUST precede the is_linux branch
# (b1nix is is_linux via C1), so we insert the b1nix arm before it.
F="$SRC/build/config/BUILD.gn"
if ! grep -q 'b1nix: pthread/dl/rt are folded into' "$F"; then
  perl -0777 -i -pe 's~(  \} else if \(is_linux \|\| is_chromeos\) \{\n)(    libs = \[\n      "dl",\n      "pthread",\n      "rt",\n    \])~  } else if (current_os == "b1nix") {\n    # b1nix: pthread/dl/rt are folded into libc.a/libb1nix.a (linked implicitly).\n    libs = []\n${1}${2}~' "$F"
  grep -q 'b1nix: pthread/dl/rt are folded into' "$F" || die "Patch C10 anchor not found in $F"
  echo "Patch C10 applied: build/config/BUILD.gn (empty default_libs for b1nix)"
else echo "Patch C10 already present"; fi

# --- Patch C11: compiler/BUILD.gn — define __linux__ for the b1nix target -----
# b1nix is deliberately linux-ABI-shaped. Its cross GCC predefines __b1nix__ /
# __unix__ but NOT __linux__, so the thousands of third_party `#if defined
# (__linux__)` OS checks (perfetto, abseil, skia, ...) fall through to "unknown
# OS" and fail to compile. Define __linux__ (and __linux) for the b1nix target
# so those select the linux path — the preprocessor-level twin of the is_linux
# GN alias (C1) and OS_LINUX (Patch 14). Conflict-free: the b1nix sysroot headers
# do not key on __linux__, and -Wno-builtin-macro-redefined is already set.
F="$BUILD/config/compiler/BUILD.gn"
if ! grep -q 'b1nix is linux-ABI-shaped: define __linux__' "$F"; then
  perl -0777 -i -pe 's~(  if \(is_linux \|\| is_chromeos\) \{\n    if \(target_os != "b1nix"\) \{  # b1nix: pthread lives in libb1nix.a; GCC has no -pthread\n      cflags \+= \[ "-pthread" \]\n    \})~${1}\n    if (target_os == "b1nix") {\n      # b1nix is linux-ABI-shaped: define __linux__ so the thousands of\n      # third_party `#if defined(__linux__)` OS checks select the linux path\n      # (mirrors the is_linux GN alias / OS_LINUX in build_config.h). The b1nix\n      # cross GCC does not predefine __linux__, and the b1nix sysroot headers do\n      # not key on it, so this is conflict-free. -Wno-builtin-macro-redefined is\n      # already set by the build.\n      defines += [\n        "__linux__=1",\n        "__linux=1",\n      ]\n    }~' "$F"
  grep -q 'b1nix is linux-ABI-shaped: define __linux__' "$F" || die "Patch C11 anchor not found in $F"
  echo "Patch C11 applied: compiler/BUILD.gn (__linux__ define for b1nix)"
else echo "Patch C11 already present"; fi

# --- Patch C12: treat_warnings_as_errors — GCC -Wno-error relaxations for b1nix
# b1nix builds with GCC, stricter than the clang Chromium targets: it flags
# warnings clang does not (sign-compare, unused-function/variable,
# maybe-uninitialized, ...) in third_party code that are NOT real bugs. With
# -Werror these become hard errors. Demote those GCC-only diagnostics to
# warnings for the b1nix build so the GCC port compiles (they are still emitted).
F="$BUILD/config/compiler/BUILD.gn"
if ! grep -q 'b1nix builds with GCC, which is stricter' "$F"; then
  perl -0777 -i -pe 's~(  \} else \{\n    cflags = \[ "-Werror" \]\n)~${1}\n    if (target_os == "b1nix" \&\& !is_clang) {\n      # b1nix builds with GCC, which is stricter than the clang Chromium targets\n      # and flags warnings clang does not (and which are not real bugs in this\n      # third_party code). Demote those GCC-only diagnostics from errors so the\n      # GCC port compiles; they are still emitted as warnings.\n      cflags += [\n        "-Wno-error=sign-compare",\n        "-Wno-error=unused-function",\n        "-Wno-error=unused-variable",\n        "-Wno-error=unused-but-set-variable",\n        "-Wno-error=maybe-uninitialized",\n        "-Wno-error=nonnull",\n        "-Wno-error=redundant-move",\n        "-Wno-error=deprecated-declarations",\n      ]\n    }\n~' "$F"
  grep -q 'b1nix builds with GCC, which is stricter' "$F" || die "Patch C12 anchor not found in $F"
  echo "Patch C12 applied: treat_warnings_as_errors (GCC -Wno-error for b1nix)"
else echo "Patch C12 already present"; fi

echo "b1nix //build patches applied to $SRC"
