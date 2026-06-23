#!/bin/sh
# apply.sh — stage the M68 b1nix-host patch into a rust-lang/rust source tree.
# Usage: tools/patches/rust/apply.sh /path/to/rust-src-full
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:?usage: apply.sh <rust-src-full>}"
SPEC="$SRC/compiler/rustc_target/src/spec"

# 1. drop the built-in b1nix host target
cp "$HERE/x86_64_unknown_b1nix.rs" "$SPEC/targets/x86_64_unknown_b1nix.rs"

# 2. register it in the supported_targets! table (idempotent)
python3 - "$SPEC/mod.rs" <<'PY'
import sys
f=sys.argv[1]; s=open(f).read()
anchor='    ("x86_64-unknown-linux-musl", x86_64_unknown_linux_musl),\n'
add='    ("x86_64-unknown-b1nix", x86_64_unknown_b1nix),\n'
if add not in s:
    assert anchor in s, "anchor missing in mod.rs"
    s=s.replace(anchor, anchor+add, 1); open(f,"w").write(s)
print("registered")
PY

# 3. bootstrap.toml (edit the CROSS paths inside before building)
[ -f "$SRC/bootstrap.toml" ] || cp "$HERE/bootstrap.toml.sample" "$SRC/bootstrap.toml"

# 4. bootstrap: map x86_64-unknown-b1nix -> CMAKE_SYSTEM_NAME=Linux so the LLVM
#    cross-build enables LLVM_ON_UNIX and the POSIX paths.
( cd "$SRC" && git apply --reverse --check "$HERE/bootstrap-llvm-b1nix-system-name.patch" 2>/dev/null \
  || git apply "$HERE/bootstrap-llvm-b1nix-system-name.patch" ) || \
  echo "WARN: apply the bootstrap llvm.rs b1nix CMAKE_SYSTEM_NAME branch manually"
echo ">> staged b1nix host target into $SRC"
echo ">> NOTE: edit $SRC/bootstrap.toml [target.x86_64-unknown-b1nix] cc/cxx/ar paths to your cross bin"
