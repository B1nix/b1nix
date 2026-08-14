#!/bin/sh
# Print the licence of every Alpine package the image ships, from the package
# index itself.
#
# The third-party inventory used to be a hand-kept table, and it drifted: it
# still listed Mesa, TinyGL and NetSurf months after they left the tree, and it
# said nothing about the 250-odd Alpine packages that arrived to replace them.
# A list maintained by hand about a set chosen by a lock file will always drift,
# so this reads both: the packages from tools/packages/alpine.lock, the licence
# and description from the APKINDEX that fetch already downloads.
#
# Usage:
#   sh tools/packages/licenses.sh            # markdown table, for the inventory
#   sh tools/packages/licenses.sh --check    # exit 1 if any package has no licence
#
# SPDX-License-Identifier: GPL-2.0-only

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${B1NIX_ARCH:-x86_64}"
LOCK="$ROOT_DIR/tools/packages/alpine.lock"
CACHE="$ROOT_DIR/build/$ARCH/pkgcache"

MODE="${1:-table}"

[ -f "$LOCK" ] || { echo "licenses: no lock file at $LOCK" >&2; exit 1; }
[ -d "$CACHE" ] || {
  echo "licenses: no package cache at $CACHE — run a fetch first" >&2
  exit 1
}

INDEXES=$(find "$CACHE" -name APKINDEX -type f | sort)
[ -n "$INDEXES" ] || {
  echo "licenses: no APKINDEX under $CACHE — run a fetch first" >&2
  exit 1
}

awk -v mode="$MODE" '
  # Pass 1: the lock file names the packages and pins their versions.
  FNR == NR {
    if ($1 != "" && $1 !~ /^#/) { want[$1] = $2 }
    next
  }
  # Pass 2: APKINDEX records, one blank-line-separated stanza per package.
  /^P:/ { pkg = substr($0, 3) }
  /^V:/ { ver = substr($0, 3) }
  /^L:/ { lic = substr($0, 3) }
  /^T:/ { desc = substr($0, 3) }
  /^$/ {
    if (pkg != "" && (pkg in want)) {
      if (!(pkg in seen)) {
        seen[pkg] = 1
        license[pkg] = (lic == "" ? "(none recorded)" : lic)
        version[pkg] = ver
        summary[pkg] = desc
      }
    }
    pkg = ""; ver = ""; lic = ""; desc = ""
  }
  END {
    missing = 0
    if (mode == "table") {
      printf "| Package | Version | License | Description |\n"
      printf "| --- | ---: | --- | --- |\n"
    }
    n = asorti(want, order)
    for (i = 1; i <= n; i++) {
      p = order[i]
      if (!(p in seen)) { printf "%s: not in any APKINDEX\n", p > "/dev/stderr"; missing++; continue }
      if (license[p] == "(none recorded)") { missing++ }
      if (mode == "table")
        printf "| `%s` | %s | %s | %s |\n", p, version[p], license[p], summary[p]
    }
    if (mode == "--check") {
      if (missing > 0) { printf "licenses: %d package(s) without a recorded licence\n", missing > "/dev/stderr"; exit 1 }
      printf "licenses: %d packages, every one carries a licence\n", n
    }
  }
' "$LOCK" $INDEXES
