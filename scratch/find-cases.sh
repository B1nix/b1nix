#!/usr/bin/env bash
set -euo pipefail
# Find the closest case statement before 68264 in configure
grep -n "case " /root/b1nix-toolchain/src/gcc-13.2.0/libstdc++-v3/configure | awk -F: '$1 < 68264 {val=$0} END {print val}'
