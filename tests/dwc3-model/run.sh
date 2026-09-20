#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build kernel/dev/dwc3_gadget.c on the host against the controller model in
# model.c and run it. See model.c.
set -e
d=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/dwc3-model.$$
cc -std=c11 -O1 -g -Wall -Wno-unused-function -I"$d/include" -o "$out" "$d/model.c"
"$out"; rc=$?
rm -f "$out"
exit $rc
