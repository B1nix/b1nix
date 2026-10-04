#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# A small NTFS volume, for the module tests that need a mounted filesystem
# which comes FROM a module.
#
# What those tests check is that a mounted filesystem pins the module that
# provides it. That was btrfs, then isofs; both are built into the kernel now
# that the imported Linux implementations replaced ours, and ntfs is the
# filesystem module still there to ask.
#
# No mkntfs is assumed on the build host (ntfs-3g is rarely installed), so the
# volume is laid out here, structure by structure, as NTFS 3.1 defines it: a
# boot sector, a $MFT whose record 0 describes the MFT's own clusters with a
# non-resident $DATA run list, the root directory (record 5) with a $I30
# $INDEX_ROOT, and one file, hello.txt (record 16), with resident data. Every
# record carries its update sequence array, applied as on disk. It is a
# minimal volume rather than a complete one -- no $Bitmap, $LogFile or $Secure
# -- which is everything a read-only driver walks: the test then reads
# hello.txt back through the mount and compares it, so a driver that accepted
# the boot sector alone would not pass.
set -eu
BUILD_DIR="${1:?usage: mk-ntfs-test-image.sh <build-dir>}"
IMG="$BUILD_DIR/ntfs-test.img"
REV=1

if [ -f "$IMG" ] && [ "$(cat "$IMG.rev" 2>/dev/null)" = "$REV" ]; then
	exit 0
fi
command -v python3 >/dev/null 2>&1 || exit 0
rm -f "$IMG" "$IMG.rev"

python3 - "$IMG.tmp" <<'EOF'
import struct, sys

SECTOR = 512
SPC = 8                         # sectors per cluster
CLUSTER = SECTOR * SPC          # 4096
VOLUME = 1024 * 1024
REC = 1024                      # MFT record size (clusters-per-record = -10)
MFT_LCN = 4
MFT_RECS = 32
MFTMIRR_LCN = MFT_LCN + MFT_RECS * REC // CLUSTER
HELLO = b"ntfs test volume\n"
SEQ = 1

img = bytearray(VOLUME)

# ── boot sector ──
bs = bytearray(SECTOR)
bs[0:3] = b"\xeb\x52\x90"
bs[3:11] = b"NTFS    "
struct.pack_into("<HB", bs, 0x0B, SECTOR, SPC)
bs[0x15] = 0xF8                                  # media: fixed disk
struct.pack_into("<HHI", bs, 0x18, 63, 255, 0)   # geometry, hidden sectors
struct.pack_into("<Q", bs, 0x28, VOLUME // SECTOR - 1)
struct.pack_into("<QQ", bs, 0x30, MFT_LCN, MFTMIRR_LCN)
struct.pack_into("<b", bs, 0x40, -10)            # 2^10 = 1024-byte records
struct.pack_into("<b", bs, 0x44, 1)              # one cluster per index block
struct.pack_into("<Q", bs, 0x48, 0xB1A1C5E7F00D1234)
bs[510:512] = b"\x55\xaa"
img[0:SECTOR] = bs

def mref(rec):
    return rec | (SEQ << 48)

def resident(atype, value, ident, name=""):
    nm = name.encode("utf-16-le")
    name_off = 0x18
    val_off = (name_off + len(nm) + 7) & ~7
    length = (val_off + len(value) + 7) & ~7
    a = bytearray(length)
    struct.pack_into("<IIBBHHH", a, 0, atype, length, 0, len(name), name_off,
                     0, ident)
    struct.pack_into("<IHBB", a, 0x10, len(value), val_off,
                     1 if atype == 0x30 else 0, 0)
    a[name_off:name_off + len(nm)] = nm
    a[val_off:val_off + len(value)] = value
    return bytes(a)

def nonresident(atype, runlist, clusters, size, ident):
    runs_off = 0x40
    length = (runs_off + len(runlist) + 1 + 7) & ~7
    a = bytearray(length)
    struct.pack_into("<IIBBHHH", a, 0, atype, length, 1, 0, runs_off, 0, ident)
    struct.pack_into("<QQHH", a, 0x10, 0, clusters - 1, runs_off, 0)
    struct.pack_into("<QQQ", a, 0x28, clusters * CLUSTER, size, size)
    a[runs_off:runs_off + len(runlist)] = runlist
    return bytes(a)

TIME = 132000000000000000      # 2019, in 100 ns units since 1601

def std_info():
    return struct.pack("<QQQQIIII", TIME, TIME, TIME, TIME, 0, 0, 0, 0)

def file_name(parent, name, size, is_dir=False):
    n = name.encode("utf-16-le")
    flags = 0x10000000 if is_dir else 0x20       # directory / archive
    return struct.pack("<QQQQQQQII", mref(parent), TIME, TIME, TIME, TIME,
                       size, size, flags, 0) + \
        struct.pack("<BB", len(name), 1) + n      # namespace 1: Win32

def record(number, flags, attrs):
    r = bytearray(REC)
    usa_off, usa_count = 0x30, REC // SECTOR + 1
    first = (usa_off + usa_count * 2 + 7) & ~7
    off = first
    for i, a in enumerate(attrs):
        r[off:off + len(a)] = a
        off += len(a)
    struct.pack_into("<I", r, off, 0xFFFFFFFF)
    used = off + 8
    struct.pack_into("<4sHHQHHHHIIQHHI", r, 0, b"FILE", usa_off, usa_count, 0,
                     SEQ, 1, first, flags, used, REC, 0, len(attrs), 0,
                     number)
    # The update sequence: the last two bytes of every sector move into the
    # array, and the sequence number takes their place.
    usn = 1
    struct.pack_into("<H", r, usa_off, usn)
    for s in range(1, usa_count):
        end = s * SECTOR - 2
        r[usa_off + 2 * s:usa_off + 2 * s + 2] = r[end:end + 2]
        struct.pack_into("<H", r, end, usn)
    return bytes(r)

def put_record(n, data):
    base = MFT_LCN * CLUSTER + n * REC
    img[base:base + REC] = data
    mirr = MFTMIRR_LCN * CLUSTER + n * REC
    if n < 4:
        img[mirr:mirr + REC] = data

IN_USE, IS_DIR = 0x01, 0x02
mft_clusters = MFT_RECS * REC // CLUSTER

# record 0: $MFT, whose $DATA is the MFT itself
runlist = bytes([0x11, mft_clusters, MFT_LCN, 0x00])
put_record(0, record(0, IN_USE, [
    resident(0x10, std_info(), 0),
    resident(0x30, file_name(5, "$MFT", mft_clusters * CLUSTER), 1),
    nonresident(0x80, runlist, mft_clusters, mft_clusters * CLUSTER, 2),
]))

# record 16: hello.txt, resident data
hello_fn = file_name(5, "hello.txt", len(HELLO))
put_record(16, record(16, IN_USE, [
    resident(0x10, std_info(), 0),
    resident(0x30, hello_fn, 1),
    resident(0x80, HELLO, 2),
]))

# record 5: the root directory, indexing hello.txt by $FILE_NAME
entry_len = (0x10 + len(hello_fn) + 7) & ~7
entry = bytearray(entry_len)
struct.pack_into("<QHHI", entry, 0, mref(16), entry_len, len(hello_fn), 0)
entry[0x10:0x10 + len(hello_fn)] = hello_fn
last = struct.pack("<QHHI", 0, 0x10, 0, 0x02)
entries = bytes(entry) + last
node = struct.pack("<IIII", 0x10, 0x10 + len(entries), 0x10 + len(entries), 0)
iroot = struct.pack("<IIIB3x", 0x30, 1, CLUSTER, 1) + node + entries
put_record(5, record(5, IN_USE | IS_DIR, [
    resident(0x10, std_info(), 0),
    resident(0x30, file_name(5, ".", 0, True), 1),
    resident(0x90, iroot, 2, "$I30"),
]))

# The backup boot sector, in the volume's last sector.
img[VOLUME - SECTOR:VOLUME] = bs

with open(sys.argv[1], "wb") as f:
    f.write(img)
EOF
mv -f "$IMG.tmp" "$IMG"
echo "$REV" > "$IMG.rev"
