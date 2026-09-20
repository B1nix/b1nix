#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Index which literal smoke markers each lane log contains.

tests/smoke.sh grades ~1450 markers, one `grep -q` fork apiece, which is
seconds of a run spent after the last guest has stopped. This reads each log
once and writes, per log, the literal check_output patterns found in it. The
harness answers a pattern listed there as a pass without forking; any pattern
not listed -- absent, or not a plain literal -- still goes to grep, so the
verdicts cannot differ from grep's.

A pattern is indexed only if a substring match implies grep's match: no
backslash (shell or BRE escape), no `^` or `$` anchors, no shell variable.
`.`, `*` and brackets are safe because the literal text also satisfies them.

Usage: grade-index.py <smoke.sh> <out-dir> VAR=logpath...
Writes <out-dir>/<VAR>.hits, one pattern per line.
"""
import os
import re
import sys


def main():
    script, out_dir = sys.argv[1], sys.argv[2]
    logs = dict(a.split("=", 1) for a in sys.argv[3:])
    src = open(script, encoding="utf-8", errors="replace").read()
    pairs = re.findall(r'check_(?:output|iommu) "\$([A-Z_]+)" "([^"]*)"', src)
    wanted = {}
    for var, pat in pairs:
        if var not in logs or not pat or re.search(r'[\\^$`]', pat):
            continue
        wanted.setdefault(var, set()).add(pat)
    os.makedirs(out_dir, exist_ok=True)
    for var, path in logs.items():
        hits = []
        try:
            data = open(path, "rb").read()
        except OSError:
            data = None
        if data is not None:
            for pat in sorted(wanted.get(var, ())):
                if pat.encode() in data:
                    hits.append(pat)
        with open(os.path.join(out_dir, var + ".hits"), "w", encoding="utf-8") as f:
            f.write("\n".join(hits) + ("\n" if hits else ""))


if __name__ == "__main__":
    main()
