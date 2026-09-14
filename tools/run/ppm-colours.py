#!/usr/bin/env python3
"""Count what is actually in a frame QEMU dumped from the guest's scanout.

A screendump of a display that never drew is not empty -- it is a solid
rectangle, and a solid rectangle passes any check that only asks whether a
file exists or has a plausible size.  What separates it from a desktop is how
many distinct colours it holds: two or three for a blank or single-fill frame,
thousands for a wallpaper with a panel on it.  So the number this prints is
the assertion, and there is no way for the guest to raise it without drawing.

Prints: "<width>x<height> unique=<n> top=<rrggbb>:<share>"
Exit status is 0 whether the frame is interesting or not; the caller decides
the threshold, because "interesting" differs between a desktop and a test
pattern.
"""
import sys


def read_token(f):
    """PPM headers are whitespace-separated with '#' comments to end of line."""
    tok = b""
    while True:
        c = f.read(1)
        if not c:
            raise ValueError("truncated header")
        if c == b"#":
            while c and c != b"\n":
                c = f.read(1)
            continue
        if c.isspace():
            if tok:
                return tok
            continue
        tok += c


def main(path):
    with open(path, "rb") as f:
        magic = read_token(f)
        if magic != b"P6":
            raise ValueError("not a binary PPM: %s" % magic.decode("latin1"))
        w = int(read_token(f))
        h = int(read_token(f))
        maxv = int(read_token(f))
        if maxv != 255:
            raise ValueError("unsupported maxval %d" % maxv)
        data = f.read(w * h * 3)
    if len(data) < w * h * 3:
        raise ValueError("short pixel data: %d of %d" % (len(data), w * h * 3))
    counts = {}
    for i in range(0, len(data), 3):
        px = data[i:i + 3]
        counts[px] = counts.get(px, 0) + 1
    top, n = max(counts.items(), key=lambda kv: kv[1])
    print("%dx%d unique=%d top=%02x%02x%02x:%.3f"
          % (w, h, len(counts), top[0], top[1], top[2], n / float(w * h)))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.stderr.write("usage: ppm-colours.py <frame.ppm>\n")
        sys.exit(2)
    try:
        sys.exit(main(sys.argv[1]))
    except Exception as exc:  # a frame that cannot be parsed is a failure
        sys.stderr.write("ppm-colours: %s: %s\n" % (sys.argv[1], exc))
        sys.exit(1)
