#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Reassemble a kernel scanout dump and look for lost pages.

The kernel prints the frame the display engine is reading as `FD<n> <y> <x>
<hex>` lines (b1nix.drm-framedump*, see kernel/lkpi/i915_display_probe.c).
This turns them back into a PNG and, given a rectangle, reports runs of black
that are exactly a page long and page-aligned inside it -- the signature of a
client buffer page the compositor never saw written.

  python3 tools/run/fd-image.py <log> <frame> [out.png] [x0 y0 x1 y1]
"""
import sys, zlib, struct, re

W, H = 1920, 1080

def read_frame(log, n):
    rows = {}
    pat = re.compile(rb'FD%d (\d+) (\d+) ([0-9a-f]+)' % n)
    for line in open(log, 'rb'):
        m = pat.search(line)
        if not m:
            continue
        y, x, hx = int(m[1]), int(m[2]), m[3].decode()
        row = rows.setdefault(y, [0] * W)
        for i in range(len(hx) // 6):
            row[x + i] = int(hx[i * 6:i * 6 + 6], 16)
    return rows

def write_png(rows, path):
    def chunk(t, d):
        return (struct.pack('>I', len(d)) + t + d +
                struct.pack('>I', zlib.crc32(t + d) & 0xffffffff))
    raw = b''
    for y in range(H):
        r = rows.get(y, [0] * W)
        raw += b'\0' + b''.join(bytes(((v >> 16) & 255, (v >> 8) & 255, v & 255))
                                for v in r)
    open(path, 'wb').write(
        b'\x89PNG\r\n\x1a\n' +
        chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) +
        chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

def zero_runs(rows, x0, y0, x1, y1):
    """Black pixels, in the rectangle's own row order, grouped into runs."""
    idx = sorted((y - y0) * (x1 - x0) + (x - x0)
                 for y in range(y0, y1)
                 for x in range(x0, x1)
                 if rows.get(y, [0] * W)[x] == 0)
    runs, start, prev = [], None, None
    for i in idx:
        if prev is None or i != prev + 1:
            if start is not None:
                runs.append((start, prev))
            start = i
        prev = i
    if start is not None:
        runs.append((start, prev))
    return runs

def main():
    log, n = sys.argv[1], int(sys.argv[2])
    out = sys.argv[3] if len(sys.argv) > 3 else None
    rows = read_frame(log, n)
    print("rows %d" % len(rows))
    if out:
        write_png(rows, out)
        print("wrote %s" % out)
    if len(sys.argv) > 7:
        x0, y0, x1, y1 = (int(v) for v in sys.argv[4:8])
        bad = 0
        for s, e in zero_runs(rows, x0, y0, x1, y1):
            if e - s + 1 == 1024 and s % 1024 == 0:
                bad += 1
                print("lost page: run %d..%d (4096 bytes, page-aligned)" % (s, e))
        print("page-aligned zero runs: %d" % bad)
        sys.exit(1 if bad else 0)

main()
