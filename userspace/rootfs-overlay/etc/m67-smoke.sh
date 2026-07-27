#!/bin/sh
# M67: static Rust std smoke ELF (cross-compiled hello_b1nix.rs). Same
# start/done bracketing rationale as m40-smoke.sh.
echo "M67-RUST: start"
if [ -x /bin/m67-rust ]; then
  /bin/m67-rust
  [ $? -eq 0 ] && echo "M67-RUST: ok run-std"
fi
echo "M67-RUST: done"
