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

# --- Patch 7: rust.gni — real b1nix rust_abi_target --------------------------
# enable_rust is ON for the Chromium-with-Rust build, so b1nix must map to its
# OWN Rust target (x86_64-unknown-b1nix — the cross-rust std sysroot), NOT the
# linux triple. Dedicated branch before the is_linux map. (Also clears the
# assert(rust_abi_target != "") the coverage config trips for every target.)
F="$BUILD/config/rust.gni"
if ! grep -q '"x86_64-unknown-b1nix"' "$F"; then
  perl -0777 -i -pe 's~rust_abi_target = ""\nif \(is_linux \|\| is_chromeos(?: \|\| current_os == "b1nix")?\) \{~rust_abi_target = ""\nif (current_os == "b1nix") {\n  rust_abi_target = "x86_64-unknown-b1nix"\n} else if (is_linux || is_chromeos) {~' "$F"
  grep -q '"x86_64-unknown-b1nix"' "$F" || die "Patch 7 anchor not found in $F"
  echo "Patch 7 applied: rust.gni (b1nix rust_abi_target = x86_64-unknown-b1nix)"
else echo "Patch 7 already present"; fi

# --- Patch 7b: register x86_64-unknown-b1nix as a known Rust target triple ----
KT="$BUILD/rust/known-target-triples.txt"
if [ -f "$KT" ] && ! grep -q "x86_64-unknown-b1nix" "$KT"; then
  printf 'x86_64-unknown-b1nix\n' >> "$KT"
  echo "Patch 7b applied: known-target-triples.txt (+x86_64-unknown-b1nix)"
else echo "Patch 7b already present"; fi

# --- Patch 7c: //build/rust/std — profiler_builtins is chromium-toolchain-only -
# The b1nix cross std doesn't build profiler_builtins (coverage/PGO only); GN
# otherwise tries to copy it into the assembled sysroot and fails.
F="$BUILD/rust/std/BUILD.gn"
if grep -q '"profiler_builtins",' "$F"; then
  perl -0777 -i -pe 's~  skip_stdlib_files = \[\n    "profiler_builtins",\n    "rustc_std_workspace_alloc",\n    "rustc_std_workspace_core",\n    "rustc_std_workspace_std",\n  \]~  skip_stdlib_files = [\n    "rustc_std_workspace_alloc",\n    "rustc_std_workspace_core",\n    "rustc_std_workspace_std",\n  ]\n\n  if (use_chromium_rust_toolchain) {\n    skip_stdlib_files += [ "profiler_builtins" ]\n  }~' "$F"
  echo "Patch 7c applied: rust/std profiler_builtins gated on chromium toolchain"
else echo "Patch 7c already present"; fi

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
  perl -0777 -i -pe 's~(  \} else \{\n    cflags = \[ "-Werror" \]\n)~${1}\n    if (target_os == "b1nix" \&\& !is_clang) {\n      # b1nix builds with GCC, which is stricter than the clang Chromium targets\n      # and flags warnings clang does not (and which are not real bugs in this\n      # third_party code). Demote those GCC-only diagnostics from errors so the\n      # GCC port compiles; they are still emitted as warnings.\n      cflags += [\n        "-Wno-error=sign-compare",\n        "-Wno-error=unused-function",\n        "-Wno-error=unused-variable",\n        "-Wno-error=unused-but-set-variable",\n        "-Wno-error=maybe-uninitialized",\n        "-Wno-error=nonnull",\n        "-Wno-error=redundant-move",\n        "-Wno-error=deprecated-declarations",\n        "-Wno-error=tautological-compare",\n        "-Wno-error=attributes",\n        "-Wno-error=changes-meaning",\n        "-Wno-error=return-type",\n        "-Wno-error=unknown-pragmas",\n        "-Wno-error=dangling-else",\n        "-Wno-error=array-bounds",\n        "-Wno-error=range-loop-construct",\n        "-Wno-error=format-truncation",\n      ]\n    }\n~' "$F"
  grep -q 'b1nix builds with GCC, which is stricter' "$F" || die "Patch C12 anchor not found in $F"
  echo "Patch C12 applied: treat_warnings_as_errors (GCC -Wno-error for b1nix)"
else echo "Patch C12 already present"; fi

# --- Patch C13: filter_clang_args.py — drop GCC-only flags for bindgen --------
# bindgen runs libclang to parse C++ headers and is handed the target's {{cflags}}
# — but the b1nix target compiler is GCC, so those carry GCC-only warning options
# (-Wno-maybe-uninitialized, -Wno-packed-not-aligned, -Wno-class-memaccess, the
# C12 -Wno-error=* set, ...) that clang rejects with -Werror=unknown-warning
# -option, failing every *_bindgen_generator. Filter them out in
# filter_clang_args() (the existing libclang-arg sanitizer). They only affect
# diagnostics, never the generated bindings.
F="$SRC/build/rust/gni_impl/filter_clang_args.py"
if [ -f "$F" ] && ! grep -q 'b1nix port (M60-62): bindgen runs libclang' "$F"; then
  perl -0777 -i -pe 's~(      elif args\[i\] == .-ftime-trace.:\n        pass\n)(      else:\n        yield args\[i\])~${1}      # b1nix port (M60-62): bindgen runs libclang to parse C++, but the b1nix\n      # target compiler is GCC, so {{cflags}} carry GCC-only warning options that\n      # clang rejects with -Werror=unknown-warning-option. Drop them here so\n      # bindgen can parse the headers (these only affect diagnostics, never the\n      # generated bindings).\n      elif args[i] in (\n          "-Wno-maybe-uninitialized",\n          "-Werror=maybe-uninitialized",\n          "-Wno-error=maybe-uninitialized",\n          "-Wno-packed-not-aligned",\n          "-Wno-class-memaccess",\n          "-Wno-error=sign-compare",\n          "-Wno-error=unused-function",\n          "-Wno-error=unused-variable",\n          "-Wno-error=unused-but-set-variable",\n          "-Wno-error=nonnull",\n          "-Wno-error=redundant-move",\n          "-Wno-redundant-move",\n          "-Wno-dangling-reference",\n          "-Wno-error=tautological-compare",\n          "-Wno-error=attributes",\n          "-Wno-error=changes-meaning",\n          "-Wno-error=return-type",\n          "-fno-math-errno",\n      ):\n        pass\n${2}~' "$F"
  grep -q 'b1nix port (M60-62): bindgen runs libclang' "$F" || die "Patch C13 anchor not found in $F"
  echo "Patch C13 applied: filter_clang_args.py (drop GCC-only flags for bindgen)"
else echo "Patch C13 already present (or file missing)"; fi

# --- Patch C14: abseil direct_mmap.h — b1nix uses the mmap() fallback ----------
# abseil's DirectMmap issues a RAW mmap syscall on __linux__ (syscall(SYS_mmap)).
# b1nix defines __linux__ (C11) but its syscall NUMBERS are not Linux's, so a raw
# Linux mmap syscall would hit the wrong b1nix syscall. abseil already has a
# regular-mmap() fallback for non-linux; route b1nix to it by excluding b1nix
# from the __linux__ direct-syscall guard.
F="$SRC/third_party/abseil-cpp/absl/base/internal/direct_mmap.h"
if [ -f "$F" ] && ! grep -q 'b1nix port (M60-62): b1nix defines __linux__ but its raw syscall' "$F"; then
  perl -0777 -i -pe 's~(#ifdef ABSL_HAVE_MMAP\n\n#include <sys/mman.h>\n\n)#ifdef __linux__~${1}// b1nix port (M60-62): b1nix defines __linux__ but its raw syscall NUMBERS are\n// not the Linux ones, so abseil DirectMmap must NOT issue a direct mmap syscall.\n// Route b1nix through the regular mmap()/munmap() fallback below.\n#if defined(__linux__) \&\& !defined(__b1nix__)~' "$F"
  grep -q 'b1nix port (M60-62): b1nix defines __linux__ but its raw syscall' "$F" || die "Patch C14 anchor not found in $F"
  echo "Patch C14 applied: abseil direct_mmap.h (mmap fallback for b1nix)"
else echo "Patch C14 already present (or file missing)"; fi

# --- Patch C15: partition_alloc.gni — disable PKEYS for b1nix -----------------
# is_pkeys_available is (is_linux||is_chromeos) && x64, which is TRUE for b1nix,
# so partition_alloc compiles its thread_isolation/pkey.cc which issues
# syscall(SYS_pkey_alloc/free/pkey_mprotect). b1nix has NO memory-protection-keys
# (MPK) support and no such syscalls. Disable the feature for b1nix (honest — the
# hardware/OS feature is genuinely absent, so it must be off, not stubbed).
F="$SRC/base/allocator/partition_allocator/partition_alloc.gni"
if [ -f "$F" ] && ! grep -q 'b1nix has no memory-protection-keys' "$F"; then
  perl -0777 -i -pe 's~(is_pkeys_available =\n    \(is_linux \|\| is_chromeos\) && current_cpu == "x64" && !is_cronet_build)~is_pkeys_available =\n    (is_linux || is_chromeos) \&\& current_cpu == "x64" \&\& !is_cronet_build \&\&\n    target_os != "b1nix"  # b1nix has no memory-protection-keys (MPK) support~' "$F"
  grep -q 'b1nix has no memory-protection-keys' "$F" || die "Patch C15 anchor not found in $F"
  echo "Patch C15 applied: partition_alloc.gni (disable pkeys for b1nix)"
else echo "Patch C15 already present (or file missing)"; fi

# --- Patch C16: rust_bindgen.gni — allow newer-rustc deny-by-default lints ----
# The Chromium-bundled rustc is newer than the bindgen that emitted the binding
# .rs files, and denies-by-default lints that bindgen output trips (notably
# unnecessary_transmutes in generated bitfield accessors). The generated code is
# correct; allow the lint for bindgen crates rather than regenerate.
F="$SRC/build/rust/rust_bindgen.gni"
if [ -f "$F" ] && ! grep -q 'unnecessary_transmutes' "$F"; then
  perl -0777 -i -pe 's~(      "-Anon_upper_case_globals",\n)(    \])~${1}\n      # Bundled rustc is newer than the bindgen that generated these bindings;\n      # allow the deny-by-default lints its output trips (correct code).\n      "-Aunnecessary_transmutes",\n${2}~' "$F"
  grep -q 'unnecessary_transmutes' "$F" || die "Patch C16 anchor not found in $F"
  echo "Patch C16 applied: rust_bindgen.gni (allow bindgen lints for b1nix)"
else echo "Patch C16 already present (or file missing)"; fi

# --- Patch C17: base/numerics CheckOnFailure::HandleFailure constexpr ---------
# GCC 13's constexpr evaluator requires the never-taken failure branch of
# checked_cast() to be a constexpr-callable function. CheckOnFailure::
# HandleFailure (which just traps) was a plain static fn, so any constant
# expression using checked_cast (e.g. base::ByteSize KiBU(1)) was rejected as
# "not a constant expression". Marking it constexpr is correct (it still traps
# when actually evaluated) and matches newer toolchains. (Partial: deeper
# CheckedNumeric constexpr gaps remain on GCC 13; see PORT-PLAN.)
F="$SRC/base/numerics/safe_conversions_impl.h"
if [ -f "$F" ] && ! grep -q 'static constexpr T HandleFailure' "$F"; then
  perl -0777 -i -pe 's~(  template <typename T>\n)(  static T HandleFailure\(\) \{)~${1}  static constexpr T HandleFailure() {~' "$F"
  grep -q 'static constexpr T HandleFailure' "$F" || die "Patch C17 anchor not found in $F"
  echo "Patch C17 applied: safe_conversions_impl.h (HandleFailure constexpr)"
else echo "Patch C17 already present (or file missing)"; fi

# --- Patch C18: content/shell drop rust test targets (need enable_rust) -------
# content/shell/BUILD.gn defines testonly rust targets (rust_test_mojom with
# generate_rust=true, rust_test_service[_ffi]) and lists them in
# content_shell_lib deps. They require enable_rust, which is OFF for the b1nix
# headless build (no rustc target), so `gn gen` asserts. content_shell proper
# does not need these test targets: drop them from the lib deps and guard their
# definitions behind if(enable_rust).
F="$SRC/content/shell/BUILD.gn"
if [ -f "$F" ] && ! grep -q 'b1nix: rust test targets' "$F"; then
  perl -0777 -i -pe 's~    ":rust_test_mojom",\n    ":rust_test_mojom_js",\n    ":rust_test_service_ffi",\n(    ":shell_controller_mojom",)~${1}~' "$F"
  perl -0777 -i -pe 's~(mojom\("rust_test_mojom"\) \{)~# b1nix: rust test targets need enable_rust (off); content_shell does not\n# use them. Guard so gn-gen does not assert(enable_rust).\nif (enable_rust) {\n${1}~' "$F"
  # close the if(enable_rust) at EOF (the rust block runs to end of file)
  printf '}\n' >> "$F"
  grep -q 'b1nix: rust test targets' "$F" || die "Patch C18 anchor not found in $F"
  echo "Patch C18 applied: content/shell drop rust test targets"
else echo "Patch C18 already present (or file missing)"; fi

# --- Patch C19: fontconfig clang discards-qualifiers relaxation ---------------
# fontconfig's NLS-off path expands _(x)/dgettext(d,s) to (s) and assigns the
# resulting const char* to char* fields; the bundled clang (host toolchain
# fontconfig build) flags this with -Werror=incompatible-pointer-types-
# discards-qualifiers. Upstream fontconfig code, not a real bug.
F="$SRC/third_party/fontconfig/BUILD.gn"
if [ -f "$F" ] && ! grep -q 'discards-qualifiers' "$F"; then
  perl -0777 -i -pe 's~(        # Work around a pointer-to-bool conversion\.\n        "-Wno-pointer-bool-conversion",\n)(      \])~${1}\n        # fontconfig _(x)/dgettext assigns const char* to char* with NLS off;\n        # clang -Werror flags discarded qualifiers (upstream code, not a bug).\n        "-Wno-incompatible-pointer-types-discards-qualifiers",\n${2}~' "$F"
  grep -q 'discards-qualifiers' "$F" || die "Patch C19 anchor not found in $F"
  echo "Patch C19 applied: fontconfig discards-qualifiers"
else echo "Patch C19 already present (or file missing)"; fi

# --- Patch C20: base/byte_size.h consteval ctors -> constexpr ----------------
# The signed-integer ByteSize/ByteSizeDelta constructors are `consteval` so that
# out-of-range CONSTANTS fail at compile time. But the KiBU()/MiBU()/... helpers
# are `constexpr` and call ByteSize(kib) with their PARAMETER; on a C++23
# compiler P2564 escalates KiBU to an immediate function so this works. GCC 13
# does NOT implement P2564, so it rejects "kib is not a constant expression"
# and base/ fails to build (byte_size.cc + ~21 files force constexpr ByteSize).
# Relax the two ctors to `constexpr`: the value is still range-checked by
# checked_cast (now at runtime via CHECK instead of at compile time) — honest,
# correctness-preserving. Drop this once the cross toolchain is GCC 14+ (which
# implements P2564 and makes the original consteval work).
F="$SRC/base/byte_size.h"
if [ -f "$F" ] && grep -q 'consteval explicit ByteSize' "$F"; then
  perl -i -pe 's/^  consteval explicit ByteSize\(T bytes\)/  constexpr explicit ByteSize(T bytes)/' "$F"
  perl -i -pe 's/^  consteval explicit ByteSizeDelta\(T bytes\)/  constexpr explicit ByteSizeDelta(T bytes)/' "$F"
  grep -q 'consteval explicit ByteSize' "$F" && die "Patch C20 failed in $F"
  echo "Patch C20 applied: byte_size.h consteval ctors -> constexpr"
else echo "Patch C20 already present (or file missing)"; fi

# --- Patch C21: base/containers/flat_tree.h KeyT deducibility (GCC 13) -------
# flat_tree's heterogeneous-lookup methods are declared as
#   template <typename K = Key> iterator find(const KeyT<K>& key);
# where upstream `KeyT<K> = ConditionalT<is_transparent, K, Key>`. `KeyT<K>` is
# a dependent alias => a NON-deduced context, so calling find(string_view) on a
# string-keyed transparent flat_map cannot deduce K from the argument; K falls
# back to the default `Key` and the string_view->const string& conversion fails.
# Clang accepts this; GCC 13 rejects it (no match for find(string_view&)), which
# breaks base/feature_list and every transparent flat_map lookup. Make KeyT an
# identity alias (`using KeyT = K;`) so K is deduced directly from the call
# argument. For transparent comparators this is exactly upstream's K; for
# non-transparent ones it requires callers to pass the key type (Chromium's
# non-transparent flat_maps already do, and a mismatch is a compile error, not
# silent breakage). Drop once the cross toolchain is GCC 14+.
F="$SRC/base/containers/flat_tree.h"
if [ -f "$F" ] && grep -q 'ConditionalT<requires { typename KeyCompare::is_transparent; }, K, Key>' "$F"; then
  perl -0777 -i -pe 's/  template <typename K>\n  using KeyT =\n      ConditionalT<requires \{ typename KeyCompare::is_transparent; \}, K, Key>;/  template <typename K>\n  using KeyT = K;  \/\/ b1nix\/GCC-13 (C21): keep K deducible for hetero lookup/' "$F"
  grep -q 'using KeyT = K;  // b1nix' "$F" || die "Patch C21 anchor not found in $F"
  echo "Patch C21 applied: flat_tree.h KeyT identity (GCC-13 deducibility)"
else echo "Patch C21 already present (or file missing)"; fi

# --- Patch C23: third_party/expat/BUILD.gn — build bundled expat for b1nix ---
# For is_linux (which b1nix matches) expat's BUILD.gn assumes a SYSTEM libexpat
# (`config expat_config { libs = ["expat"] }`, no include dir), so skia's
# SkXMLParser/SkFontMgr_android_parser can't find <expat.h>. b1nix has no system
# expat; exclude it from that branch so it builds expat from the bundled source
# (the `else` branch that exports the include dir + XML_STATIC).
F="$SRC/third_party/expat/BUILD.gn"
if [ -f "$F" ] && ! grep -q 'b1nix has no system libexpat' "$F"; then
  perl -0777 -i -pe 's~if \(\(\(is_linux && !is_castos\) \|\| is_chromeos\) && !use_fuzzing_engine\) \{~if ((((is_linux && !is_castos) || is_chromeos) && !use_fuzzing_engine) &&\n    target_os != "b1nix") {  # b1nix has no system libexpat: build from bundled source~' "$F"
  grep -q 'b1nix has no system libexpat' "$F" || die "Patch C23 anchor not found in $F"
  echo "Patch C23 applied: expat/BUILD.gn (build bundled expat for b1nix)"
else echo "Patch C23 already present (or file missing)"; fi

# --- Patch C24: net/features.gni — use_kerberos=false for b1nix --------------
# b1nix has no GSSAPI/Kerberos library, so <gssapi.h> is unavailable and
# Negotiate auth is unsupported. Default use_kerberos off for b1nix (Chromium
# supports building without it). This is the reproducible form of the args.gn
# `use_kerberos = false` override.
F="$SRC/net/features.gni"
if [ -f "$F" ] && ! grep -q 'b1nix has no GSSAPI' "$F"; then
  perl -0777 -i -pe 's~(  use_kerberos = !is_ios && !is_fuchsia && !is_castos && !is_cast_android)~$1 &&\n      target_os != "b1nix"  # b1nix has no GSSAPI/Kerberos library~' "$F"
  grep -q 'b1nix has no GSSAPI' "$F" || die "Patch C24 anchor not found in $F"
  echo "Patch C24 applied: net/features.gni (use_kerberos=false for b1nix)"
else echo "Patch C24 already present (or file missing)"; fi

echo "b1nix //build patches applied to $SRC"

# --- Patch C-LSS: don't use third_party/lss on b1nix --------------------------
# lss issues raw syscalls with REAL Linux numbers (and #defines __NR_* to them),
# but b1nix has its own syscall numbers. Exclude b1nix from the lss include in
# rand_util_posix; b1nix then uses the libc syscall() + b1nix __NR_ aliases.
for F in "$SRC/base/rand_util_posix.cc" "$SRC/base/allocator/partition_allocator/src/partition_alloc/partition_alloc_base/rand_util_posix.cc"; do
  if [ -f "$F" ] && ! grep -q "!defined(__b1nix__)" "$F"; then
    perl -0777 -i -pe 's~#if BUILDFLAG\(IS_LINUX\) \|\| BUILDFLAG\(IS_CHROMEOS\)\n(#include "third_party/lss/linux_syscall_support.h")~#if (BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)) \&\& !defined(__b1nix__)\n${1}~' "$F"
    echo "Patch C-LSS applied: $F"
  fi
done

# --- Patch C25: stack_copier_signal — pass the raw thread id to syscall -------
# base::PlatformThreadId is a class with no implicit long conversion. glibc's
# variadic syscall(long, ...) accepts it by passing the bytes through `...`;
# b1nix's syscall macro casts each arg to (long), which a class without a
# conversion operator can't satisfy. Extract the integer with .raw() (what every
# other Chromium syscall/proc path already does).
F="$SRC/base/profiler/stack_copier_signal.cc"
if [ -f "$F" ] && ! grep -q 'GetThreadId().raw()' "$F"; then
  perl -0777 -i -pe 's~syscall\(SYS_tgkill, getpid\(\), thread_delegate_->GetThreadId\(\),~syscall(SYS_tgkill, getpid(), thread_delegate_->GetThreadId().raw(),~' "$F"
  grep -q 'GetThreadId().raw()' "$F" || die "Patch C25 anchor not found in $F"
  echo "Patch C25 applied: stack_copier_signal.cc (raw thread id to tgkill)"
else echo "Patch C25 already present (or file missing)"; fi

# --- Patch C26: crypto/features.gni — use_nss_certs=false for b1nix -----------
# b1nix has no system NSS library, and crypto/nss_util.cc pulls real NSS headers
# whose PRInt64 (long) clashes with b1nix int64_t (long long). Use Chromium's
# built-in cert verifier instead (the modern default on most platforms).
F="$SRC/crypto/features.gni"
if [ -f "$F" ] && ! grep -q 'b1nix has no system NSS' "$F"; then
  perl -0777 -i -pe 's~(use_nss_certs = is_linux \|\| is_chromeos)~use_nss_certs = (is_linux || is_chromeos) && target_os != "b1nix"  # b1nix has no system NSS; built-in cert verifier~' "$F"
  grep -q 'b1nix has no system NSS' "$F" || die "Patch C26 anchor not found in $F"
  echo "Patch C26 applied: crypto/features.gni (use_nss_certs=false for b1nix)"
else echo "Patch C26 already present (or file missing)"; fi

# --- Patch C27: crashpad lss -> b1nix libc shim ------------------------------
# crashpad's client+util use linux-syscall-support (lss), which issues RAW
# Linux-numbered syscalls — wrong on b1nix. Drop in a shim that forwards the
# small sys_* subset crashpad uses to b1nix libc. (crashpad can't actually run
# on b1nix; this only lets it compile+link.)
CPATCH="$(cd "$(dirname "$0")/files" && pwd)"
LSSDIR="$SRC/third_party/crashpad/crashpad/third_party/lss"
if [ -d "$LSSDIR" ]; then
  cp -f "$CPATCH/lss_b1nix.h" "$LSSDIR/lss_b1nix.h"
  F="$LSSDIR/lss.h"
  if ! grep -q '__b1nix__' "$F"; then
    perl -0777 -i -pe 's~#if defined\(CRASHPAD_LSS_SOURCE_EXTERNAL\)~#if defined(__b1nix__)\n#include "lss_b1nix.h"\n#elif defined(CRASHPAD_LSS_SOURCE_EXTERNAL)~' "$F"
    grep -q '__b1nix__' "$F" || die "Patch C27 anchor not found in $F"
    echo "Patch C27 applied: crashpad lss.h (b1nix shim)"
  else echo "Patch C27 already present"; fi
fi

# --- Patch C28: don't build the out-of-process crashpad handler on b1nix -----
# The handler executable needs the full lss surface (ptrace/coredump/proc-task)
# that b1nix lacks; skip it. crashpad_linux.cc still compiles (the in-process
# client API), it just has no handler to spawn — fine, crash capture is a no-op.
F="$SRC/components/crash/core/app/BUILD.gn"
if [ -f "$F" ] && ! grep -q 'b1nix: no out-of-process handler' "$F"; then
  perl -0777 -i -pe 's~(    sources \+= \[ "crashpad_linux\.cc" \]\n)    data_deps = \[ ":chrome_crashpad_handler" \]~${1}    if (target_os != "b1nix") {  # b1nix: no out-of-process handler\n      data_deps = [ ":chrome_crashpad_handler" ]\n    }~' "$F"
  grep -q 'b1nix: no out-of-process handler' "$F" || die "Patch C28 anchor not found in $F"
  echo "Patch C28 applied: components/crash app (no handler on b1nix)"
else echo "Patch C28 already present (or file missing)"; fi

# --- Patch C29: angle use_libpci=false for b1nix -----------------------------
# ANGLE's GPU-info collector uses libpci on Linux+ozone; b1nix has no libpci (and
# is headless/SwiftShader). Disable it so SystemInfo_libpci.cpp is dropped and
# ANGLE uses its non-libpci GPU-detection fallback.
F="$SRC/third_party/angle/BUILD.gn"
if [ -f "$F" ] && ! grep -q 'b1nix has no libpci' "$F"; then
  perl -0777 -i -pe 's~(use_libpci =\n      \(is_linux \|\| is_chromeos\) &&\n      \(angle_use_x11 \|\| angle_use_wayland \|\| use_ozone\) && angle_has_build)~${1} &&\n      target_os != "b1nix"  # b1nix has no libpci~' "$F"
  grep -q 'b1nix has no libpci' "$F" || die "Patch C29 anchor not found in $F"
  echo "Patch C29 applied: angle/BUILD.gn (use_libpci=false for b1nix)"
else echo "Patch C29 already present (or file missing)"; fi

# --- Patch C30: field_trial_config platform — map b1nix to linux -------------
# fieldtrial_to_struct.py only accepts known platforms; b1nix isn't one. b1nix is
# linux-like, so generate the variations testing config for the "linux" platform.
F="$SRC/components/variations/field_trial_config/BUILD.gn"
if [ -f "$F" ] && ! grep -q 'b1nix maps to the linux variations platform' "$F"; then
  perl -0777 -i -pe 's~(  if \(current_os == "win"\) \{\n    platform = "windows"\n)  \} else \{~${1}  } else if (current_os == "b1nix") {\n    platform = "linux"  # b1nix maps to the linux variations platform\n  } else {~' "$F"
  grep -q 'b1nix maps to the linux variations platform' "$F" || die "Patch C30 anchor not found in $F"
  echo "Patch C30 applied: field_trial_config (b1nix->linux platform)"
else echo "Patch C30 already present (or file missing)"; fi

# --- Patch C31: media — no ALSA/PulseAudio on b1nix --------------------------
# b1nix has no audio server; the POSIX branch in media_options.gni would set
# use_alsa/use_pulseaudio=true (and pull <alsa/asoundlib.h>/<pulse/pulseaudio.h>).
# Exclude b1nix so audio uses the dummy backend.
F="$SRC/media/media_options.gni"
if [ -f "$F" ] && ! grep -q 'target_os != "b1nix"' "$F"; then
  perl -0777 -i -pe 's~(if \(is_posix && !is_android && !is_apple &&)\n(      \(!is_castos)~${1} target_os != "b1nix" &&\n${2}~' "$F"
  grep -q 'target_os != "b1nix"' "$F" || die "Patch C31 anchor not found in $F"
  echo "Patch C31 applied: media_options.gni (no alsa/pulse on b1nix)"
else echo "Patch C31 already present (or file missing)"; fi

# --- Patch C32: use_udev=false for b1nix -------------------------------------
# b1nix has no libudev; Chromium falls back to non-udev device enumeration.
F="$SRC/build/config/features.gni"
if [ -f "$F" ] && ! grep -q 'b1nix has no libudev' "$F"; then
  perl -0777 -i -pe 's~use_udev = \(is_linux && !is_castos\) \|\| is_chromeos~use_udev = ((is_linux \&\& !is_castos) || is_chromeos) \&\& target_os != "b1nix"  # b1nix has no libudev~' "$F"
  grep -q 'b1nix has no libudev' "$F" || die "Patch C32 anchor not found in $F"
  echo "Patch C32 applied: features.gni (use_udev=false for b1nix)"
else echo "Patch C32 already present (or file missing)"; fi

# --- Patch C33: perfetto LegacyTraceId overloads for b1nix -------------------
# perfetto adds size_t/intptr_t LegacyTraceId ctors only on Apple (and unsigned
# long on Win), assuming elsewhere uint64_t==unsigned long. b1nix's uint64_t is
# `unsigned long long` while size_t/long are `long`, so an `unsigned long`/`long`
# arg is ambiguous between the uint64_t and int64_t ctors. b1nix has Apple-like
# type distinctness — extend the Apple overload block to it.
F="$SRC/third_party/perfetto/include/perfetto/tracing/internal/track_event_legacy.h"
if [ -f "$F" ] && ! grep -q 'defined(__b1nix__)' "$F"; then
  perl -0777 -i -pe 's~#if PERFETTO_BUILDFLAG\(PERFETTO_OS_APPLE\)\n(  explicit LegacyTraceId\(size_t raw_id\))~#if PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE) || defined(__b1nix__)\n${1}~' "$F"
  grep -q 'defined(__b1nix__)' "$F" || die "Patch C33 anchor not found in $F"
  echo "Patch C33 applied: perfetto track_event_legacy.h (b1nix LegacyTraceId overloads)"
else echo "Patch C33 already present (or file missing)"; fi

# --- Patch C34: v8config V8_TARGET_OS_LINUX=1 for b1nix ----------------------
# v8config auto-detects b1nix as Linux (via __linux__) and defines the *presence*
# macro `#define V8_TARGET_OS_LINUX` (empty). But some bundled V8 wasm headers
# (std-object-sizes.h, wasm-objects.cc, ...) use it as a VALUE in #if expressions,
# so the empty token breaks them ("invalid token"/"expected value"). Give it a
# real `1` value (UNCONDITIONAL — fixes BOTH the b1nix-target and the host
# clang_x64 V8 builds, which both hit this auto-detect path; the host build is
# even compiled with -DDEBUG so the DEBUG-gated std-object-sizes block is live).
# `defined(V8_TARGET_OS_LINUX)` uses keep working.
F="$SRC/v8/include/v8config.h"
if [ -f "$F" ] && ! grep -q 'V8_TARGET_OS_LINUX 1' "$F"; then
  perl -0777 -i -pe 's~#ifdef V8_OS_LINUX\n# define V8_TARGET_OS_LINUX\n#endif~#ifdef V8_OS_LINUX\n# define V8_TARGET_OS_LINUX 1\n#endif~' "$F"
  grep -q 'V8_TARGET_OS_LINUX 1' "$F" || die "Patch C34 anchor not found in $F"
  echo "Patch C34 applied: v8config.h (V8_TARGET_OS_LINUX=1, host+target)"
else echo "Patch C34 already present"; fi

# --- Patch C35: disable V4L2 video-capture backend for b1nix -----------------
# media/capture/video/linux (V4L2 camera capture) needs <linux/videodev2.h> (150+
# V4L2 symbols) — b1nix has no camera/V4L2 and headless content_shell needs none.
# Drop the video/linux dep and make the platform factory return the fake one.
F="$SRC/media/capture/BUILD.gn"
if [ -f "$F" ] && ! grep -q 'target_os != "b1nix"' "$F"; then
  perl -0777 -i -pe 's~  if \(is_linux \|\| is_chromeos\) \{\n    deps \+= \[ "video/linux" \]~  if ((is_linux || is_chromeos) && target_os != "b1nix") {\n    deps += [ "video/linux" ]~' "$F"
  grep -q 'target_os != "b1nix"' "$F" || die "Patch C35a anchor not found in $F"
  echo "Patch C35a applied: media/capture/BUILD.gn (drop video/linux for b1nix)"
else echo "Patch C35a already present"; fi
F="$SRC/media/capture/video/create_video_capture_device_factory.cc"
if [ -f "$F" ] && ! grep -q '__b1nix__' "$F"; then
  # (a) don't include the V4L2 linux factory header on b1nix
  perl -0777 -i -pe 's~#if BUILDFLAG\(IS_LINUX\)\n#include "media/capture/video/linux/video_capture_device_factory_linux.h"~#if BUILDFLAG(IS_LINUX) && !defined(__b1nix__)\n#include "media/capture/video/linux/video_capture_device_factory_linux.h"~' "$F"
  # (b) b1nix returns the fake factory (no V4L2)
  perl -0777 -i -pe 's~#if BUILDFLAG\(IS_LINUX\)\n  return std::make_unique<VideoCaptureDeviceFactoryLinux>\(ui_task_runner\);~#if defined(__b1nix__)\n  return CreateFakeVideoCaptureDeviceFactory();  // b1nix: no V4L2/camera\n#elif BUILDFLAG(IS_LINUX)\n  return std::make_unique<VideoCaptureDeviceFactoryLinux>(ui_task_runner);~' "$F"
  grep -q '__b1nix__' "$F" || die "Patch C35b anchor not found in $F"
  echo "Patch C35b applied: create_video_capture_device_factory.cc (b1nix -> fake)"
else echo "Patch C35b already present"; fi

# --- Patch C36: wasm-memory-map-descriptor.cc include-before-config ----------
# This file does `#if V8_TARGET_OS_LINUX` at the very top to guard <sys/mman.h>/
# <sys/stat.h>, BEFORE any V8 header is included. For b1nix, target_os doesn't
# match a GN branch, so V8_HAVE_TARGET_OS is unset and v8config.h only DERIVES
# V8_TARGET_OS_LINUX=1 from the V8_OS_LINUX host-OS fallback — i.e. after it's
# included. So the top guard sees it undefined (skips the includes) while the
# body guard (after the v8 headers pull in v8config.h) sees it =1 and compiles
# mmap/struct stat code with no headers -> "undeclared PROT_READ / incomplete
# struct stat". Pull in v8config.h before the top guard so both agree. Fixes
# host (clang_x64) and target identically; recompiles this one file only.
F="$SRC/v8/src/wasm/wasm-memory-map-descriptor.cc"
if [ -f "$F" ] && ! grep -q 'b1nix: resolve V8_TARGET_OS_LINUX before the guard' "$F"; then
  perl -0777 -i -pe 's~(#if V8_TARGET_OS_LINUX\n#include <sys/mman.h>)~#include "include/v8config.h"  // b1nix: resolve V8_TARGET_OS_LINUX before the guard (host-OS fallback path)\n${1}~' "$F"
  grep -q 'b1nix: resolve V8_TARGET_OS_LINUX before the guard' "$F" || die "Patch C36 anchor not found in $F"
  echo "Patch C36 applied: wasm-memory-map-descriptor.cc (v8config before top guard)"
else echo "Patch C36 already present (or file missing)"; fi

# --- Patch C37: O_PATH for the xdg file-transfer portal ----------------------
# components/dbus/xdg/file_transfer_portal.cc opens an fd with O_PATH to pass it
# over D-Bus (SCM_RIGHTS). b1nix's <fcntl.h> has no O_PATH. The portal never runs
# on headless b1nix (no xdg-desktop-portal service), so this is compile-only.
# Define O_PATH to its real Linux value; b1nix's open() ignores the unknown bit
# (access mode 0 == O_RDONLY), which is the correct degenerate behavior here.
# ponytail: build stopgap for a keep-off feature; the durable fix (real O_PATH in
# userspace/include/fcntl.h) is deferred to the tech-debt closeout's clean rebuild
# so it doesn't dirty the widely-included fcntl.h mid-grind. Logged in debt doc.
F="$SRC/components/dbus/xdg/file_transfer_portal.cc"
if [ -f "$F" ] && ! grep -q 'O_PATH 0x200000' "$F"; then
  perl -0777 -i -pe 's~(#include <fcntl.h>)\n(#include <sys/types.h>)~${1}\n#ifndef O_PATH\n#define O_PATH 0x200000  /* b1nix: no O_PATH; real Linux value, open() treats unknown bit as O_RDONLY ref. Portal is headless-dead here. */\n#endif\n${2}~' "$F"
  grep -q 'O_PATH 0x200000' "$F" || die "Patch C37 anchor not found in $F"
  echo "Patch C37 applied: file_transfer_portal.cc (O_PATH fallback)"
else echo "Patch C37 already present (or file missing)"; fi

# --- Patch C38: seccomp system-headers use canonical Linux __NR_* on b1nix ---
# b1nix runs --no-sandbox, so the Linux sandbox (seccomp/namespaces) is DEAD code,
# but it must still compile + link: content/SandboxLinux reference it and the API
# even returns bpf_dsl::ResultExpr, so stubbing SandboxLinux out (the use_seccomp_
# bpf=false path) is infeasible. Keep seccomp ON and make it compile. The blocker:
# sandbox/linux/system_headers/linux_syscalls.h does `#include <sys/syscall.h>`
# first, and b1nix's <syscall.h> aliases __NR_foo -> SYS_FOO (b1nix's own, NON-
# Linux numbers). The per-arch seccomp header then skips its canonical Linux value
# (`#if !defined(__NR_foo)`), so b1nix numbers leak into the bpf policy switches ->
# duplicate-case collisions (SYS_FSYNC==18) + wrong/undeclared __NR_*. Skip that
# include on b1nix so the seccomp headers define pure Linux __NR_*; dead here, so
# the (Linux) numbers needn't match b1nix. Real seccomp wiring = M63.
F="$SRC/sandbox/linux/system_headers/linux_syscalls.h"
if [ -f "$F" ] && ! grep -q '__b1nix__' "$F"; then
  perl -0777 -i -pe 's~(#include <sys/syscall.h>)~#if !defined(__b1nix__)  // b1nix __NR_* alias to its own syscall numbers; let the seccomp headers define canonical Linux __NR_* (dead code, M63)\n${1}\n#endif~' "$F"
  grep -q '__b1nix__' "$F" || die "Patch C38 anchor not found in $F"
  echo "Patch C38 applied: linux_syscalls.h (canonical Linux __NR_* on b1nix)"
else echo "Patch C38 already present (or file missing)"; fi

# --- Patch C39: drop the seccomp signal-ABI static_asserts on b1nix ----------
# linux_signal.h static-asserts LINUX_SIGHUP==SIGHUP (1==7) etc. against the host
# libc. b1nix signal numbering differs from Linux by design, so these can't hold.
# They are compile-time-only checks guarding seccomp's hardcoded Linux signal
# numbers; the seccomp handler never runs on b1nix (--no-sandbox), so dropping the
# asserts is safe (the seccomp code is dead). Real signal reconciliation = M63.
F="$SRC/sandbox/linux/system_headers/linux_signal.h"
if [ -f "$F" ] && ! grep -q '__b1nix__' "$F"; then
  perl -0777 -i -pe 's~(static_assert\(LINUX_SIGHUP == SIGHUP, "LINUX_SIGHUP == SIGHUP"\);)~#if !defined(__b1nix__)  // b1nix signal numbers != Linux; seccomp is dead code here (M63)\n${1}~' "$F"
  perl -0777 -i -pe 's~(static_assert\(LINUX_SIG_DFL == SIG_DFL, "LINUX_SIG_DFL == SIG_DFL"\);)~${1}\n#endif  // !__b1nix__~' "$F"
  grep -q '__b1nix__' "$F" || die "Patch C39 anchor not found in $F"
  echo "Patch C39 applied: linux_signal.h (drop signal-ABI static_asserts on b1nix)"
else echo "Patch C39 already present (or file missing)"; fi

# --- Patch C40: use_cups=false for b1nix (no CUPS/printer) -------------------
# b1nix has no CUPS. printing/backend gates printer_status.{h,cc} (which include
# <cups/cups.h>) under use_cups, which is is_linux-true. Disable it; printing
# falls back to the non-CUPS backend. Headless content_shell needs no printer.
F="$SRC/printing/buildflags/buildflags.gni"
if [ -f "$F" ] && ! grep -q 'target_os != "b1nix"' "$F"; then
  perl -0777 -i -pe 's~(is_mac\) &&\n               !is_fuchsia)~${1} &&\n               target_os != "b1nix"  # b1nix has no CUPS/printer~' "$F"
  grep -q 'target_os != "b1nix"' "$F" || die "Patch C40 anchor not found in $F"
  echo "Patch C40 applied: buildflags.gni (use_cups=false for b1nix)"
else echo "Patch C40 already present (or file missing)"; fi

# --- Patch C41: trap.cc dead-code struct gaps on b1nix ----------------------
# The seccomp SIGSYS trap handler is dead on b1nix (--no-sandbox). It touches two
# struct members b1nix's headers don't have: ucontext::uc_sigmask and
# siginfo_t::_sifields. Do NOT change b1nix's ucontext/siginfo_t layout (kernel +
# libc must agree on them) — neutralize the dead accesses for b1nix. Real seccomp
# trap handler = M63.
F="$SRC/sandbox/linux/seccomp-bpf/trap.cc"
if [ -f "$F" ] && ! grep -q '__b1nix__' "$F"; then
  perl -0777 -i -pe 's~(  return sigismember\(const_cast<sigset_t\*>\(&ctx->uc_sigmask\), LINUX_SIGBUS\);)~#if defined(__b1nix__)\n  (void)ctx;  // b1nix ucontext has no uc_sigmask; seccomp trap is dead code (M63)\n  return false;\n#else\n${1}\n#endif~' "$F"
  perl -0777 -i -pe 's~(  memcpy\(&sigsys, &info->_sifields, sizeof\(sigsys\)\);)~#if defined(__b1nix__)\n  memset(&sigsys, 0, sizeof(sigsys));  // b1nix siginfo_t has no _sifields; dead code (M63)\n#else\n${1}\n#endif~' "$F"
  grep -q '__b1nix__' "$F" || die "Patch C41 anchor not found in $F"
  echo "Patch C41 applied: trap.cc (b1nix dead-code struct gaps)"
else echo "Patch C41 already present (or file missing)"; fi

# --- Patch C42: libc_interceptor gmtime fn-pointer on b1nix ------------------
# `g_libc_localtime` (type struct tm*(*)(const time_t*)) = gmtime fails on b1nix:
# <time.h> declares gmtime/localtime with a B1nixTimePtr wrapper param, so &gmtime
# isn't that pointer type. Wrap in a captureless lambda (converts to the exact fn
# pointer; gmtime(t) compiles via B1nixTimePtr's implicit ctor). This localtime
# interceptor is dead on b1nix (sandbox off) but must compile.
F="$SRC/sandbox/linux/services/libc_interceptor.cc"
if [ -f "$F" ] && ! grep -q '__b1nix__' "$F"; then
  perl -0777 -i -pe 's~    g_libc_localtime = gmtime;~#if defined(__b1nix__)\n    g_libc_localtime = [](const time_t* t) { return gmtime(t); };  // b1nix gmtime takes B1nixTimePtr; wrap to a (const time_t*) fn ptr\n#else\n    g_libc_localtime = gmtime;\n#endif~' "$F"
  grep -q '__b1nix__' "$F" || die "Patch C42 anchor not found in $F"
  echo "Patch C42 applied: libc_interceptor.cc (b1nix gmtime fn-pointer)"
else echo "Patch C42 already present (or file missing)"; fi

# --- Patch C43: force canonical Linux __NR_* in the seccomp arch header ------
# C38 stopped linux_syscalls.h from pulling <sys/syscall.h>, but it's not enough:
# some seccomp consumers (die.cc, the bpf_*_policy switches) get b1nix's <syscall.h>
# via OTHER include paths first, and b1nix aliases __NR_foo -> SYS_FOO (its own,
# colliding numbers) under #ifndef guards. Those win the seccomp header's
# `#if !defined(__NR_foo)` guards -> wrong/duplicate case values (SYS_FSYNC==18
# dup-case) + undeclared __NR_*. Fix: on b1nix, make the seccomp arch header
# UNCONDITIONALLY undef+redefine each __NR_* to its canonical Linux value, so it
# always wins regardless of include order. Only seccomp (dead on b1nix) code
# includes this header, so forcing Linux numbers here is safe. Real seccomp = M63.
F="$SRC/sandbox/linux/system_headers/x86_64_linux_syscalls.h"
if [ -f "$F" ] && ! grep -q '__b1nix__' "$F"; then
  perl -0777 -i -pe 's~#if !defined\((__NR_\w+)\)\n#define (__NR_\w+) (\S+)\n#endif~#if defined(__b1nix__)\n#undef $1\n#define $2 $3\n#elif !defined($1)\n#define $2 $3\n#endif~g' "$F"
  grep -q '__b1nix__' "$F" || die "Patch C43 anchor not found in $F"
  echo "Patch C43 applied: x86_64_linux_syscalls.h (force Linux __NR_* on b1nix)"
else echo "Patch C43 already present (or file missing)"; fi

# --- Patch C44: die.cc needs the seccomp __NR_* map --------------------------
# seccomp-bpf/die.cc uses __NR_exit_group / __NR_prctl but includes only
# <sys/syscall.h> (b1nix's, which lacks the Linux-only __NR_exit_group). Pull in
# the seccomp syscall map (with C43 forcing canonical Linux values). die.cc is
# dead on b1nix (--no-sandbox); it only needs to compile.
F="$SRC/sandbox/linux/seccomp-bpf/die.cc"
if [ -f "$F" ] && ! grep -q 'system_headers/linux_syscalls.h' "$F"; then
  perl -0777 -i -pe 's~(#include "sandbox/linux/services/syscall_wrappers.h")~${1}\n#include "sandbox/linux/system_headers/linux_syscalls.h"  // b1nix: __NR_exit_group/__NR_prctl (dead code, M63)~' "$F"
  grep -q 'system_headers/linux_syscalls.h' "$F" || die "Patch C44 anchor not found in $F"
  echo "Patch C44 applied: die.cc (include seccomp syscall map)"
else echo "Patch C44 already present (or file missing)"; fi

# --- Patch C45: generate_policy_source.py — conditional cloud-policy import ----
# The generated cloud_policy.proto imports policy_common_definitions.proto for the
# *PolicyProto field types, but content_shell defines no cloud policies, so
# CloudPolicySettings is empty and the import goes unused → protoc warns "Import
# ... is unused" → the build's --fatal_warnings makes it a hard error (cascades to
# every TU that includes cloud_policy.pb.h). Fix the generator to emit the import
# only when there is at least one field — correct upstream-compatible behaviour,
# not a b1nix-specific hack. (Done via inline python: the replacement text mixes
# ' and ", which a shell-quoted perl one-liner can't carry cleanly.)
F="$SRC/components/policy/tools/generate_policy_source.py"
if [ -f "$F" ]; then
  python3 - "$F" <<'PYEOF' || die "Patch C45 failed"
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
marker = 'only referenced when CloudPolicySettings has at least one field'
if marker in s:
    print("Patch C45 already present")
    sys.exit(0)
# 1) drop the unconditional import from CLOUD_POLICY_PROTO_HEAD (the CHROME_SETTINGS
#    head keeps its own commented import — disambiguated by the missing comment).
old_head = ('option go_package="chromium/policy/enterprise_management_proto";\n'
            '\nimport "policy_common_definitions.proto";\n\n\'\'\'')
new_head = 'option go_package="chromium/policy/enterprise_management_proto";\n\n\'\'\''
if s.count(old_head) != 1:
    sys.exit("C45: CLOUD_POLICY_PROTO_HEAD anchor not unique/found")
s = s.replace(old_head, new_head)
# 2) emit the import only when CloudPolicySettings will have fields.
anchor = '\n  sorted_chunk_numbers = sorted(fields.keys())'
ins = ('\n  # policy_common_definitions.proto supplies the *PolicyProto field types,'
       ' so it\n  # is only referenced when CloudPolicySettings has at least one'
       ' field. Emitting\n  # the import for an empty policy set (e.g. content_shell)'
       ' makes protoc warn\n  # "Import ... is unused", which --fatal_warnings turns'
       ' into a hard error.\n  if fields:\n'
       '    f.write(\'import "policy_common_definitions.proto";\\n\\n\')')
if s.count(anchor) != 1:
    sys.exit("C45: sorted_chunk_numbers anchor not unique/found")
s = s.replace(anchor, ins + anchor, 1)
open(p, 'w', encoding='utf-8').write(s)
print("Patch C45 applied: generate_policy_source.py (conditional cloud-policy import)")
PYEOF
else echo "Patch C45 skipped (file missing)"; fi

# --- Patch C46: generate_policy_source.py — recognise b1nix as a Linux platform
# Policies are gated on build-config platform tokens (PLATFORM_STRINGS); b1nix has
# no entry, so every policy is "unsupported" → the generated cloud_policy.proto and
# policy_constants.cc come out EMPTY (zero-length arrays, missing key:: constants),
# breaking ~19 policy consumers (policy_map, *_policy_handler, search_engines, ...).
# b1nix is Linux-like (OS_LINUX in base/), so map it to 'linux' next to the existing
# chromeos→chrome_os normalisation. This is the real root fix; C45 only handled the
# now-unreachable empty-policy edge.
F="$SRC/components/policy/tools/generate_policy_source.py"
if [ -f "$F" ]; then
  python3 - "$F" <<'PYEOF' || die "Patch C46 failed"
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
if "elif target_platform == 'b1nix'" in s:
    print("Patch C46 already present")
    sys.exit(0)
anchor = "  if target_platform == 'chromeos':\n    target_platform = 'chrome_os'\n"
ins = ("\n  # b1nix is a Linux-like OS (detected as OS_LINUX in base/). Policies are\n"
       "  # gated on build-config platform tokens (PLATFORM_STRINGS), which have no\n"
       "  # b1nix entry, so without this every policy is \"unsupported\" and the\n"
       "  # generated proto/constants come out empty. Treat b1nix as Linux.\n"
       "  elif target_platform == 'b1nix':\n    target_platform = 'linux'\n")
if s.count(anchor) != 1:
    sys.exit("C46: chromeos-normalization anchor not unique/found")
s = s.replace(anchor, anchor + ins, 1)
open(p, 'w', encoding='utf-8').write(s)
print("Patch C46 applied: generate_policy_source.py (map b1nix->linux for policy support)")
PYEOF
else echo "Patch C46 skipped (file missing)"; fi
