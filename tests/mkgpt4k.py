#!/usr/bin/env python3
"""Write a GPT with 4 KiB logical blocks — the layout of a phone's UFS LUN.

    mkgpt4k.py IMAGE SIZE_MIB NAME:SIZE_MIB[:FILE] [NAME:SIZE_MIB[:FILE] ...]

Partitions are laid out in order from LBA 6. A FILE, when given, is copied to
the start of its partition (an ext4 image made by mke2fs, for instance).
Protective MBR, primary header at LBA 1, entries at LBA 2, backup at the end.
"""
import struct
import sys
import uuid
import zlib

BS = 4096
LINUX_FS = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")


def main():
    img, size_mib, specs = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    total = size_mib * 1024 * 1024 // BS
    n_entries, esize = 128, 128
    entries_blocks = (n_entries * esize + BS - 1) // BS
    first_usable = 2 + entries_blocks
    last_usable = total - 2 - entries_blocks

    table = bytearray(n_entries * esize)
    lba = first_usable
    copies = []
    for i, spec in enumerate(specs):
        name, mib, *src = spec.split(":")
        blocks = int(mib) * 1024 * 1024 // BS
        if lba + blocks - 1 > last_usable:
            sys.exit(f"{name} does not fit")
        uname = name.encode("utf-16-le")[:72]
        struct.pack_into("<16s16sQQQ72s", table, i * esize, LINUX_FS.bytes_le,
                         uuid.uuid4().bytes_le, lba, lba + blocks - 1, 0, uname)
        if src:
            copies.append((lba, src[0]))
        lba += blocks

    def header(my, alt, entries_lba):
        h = bytearray(92)
        struct.pack_into("<8sIIIIQQQQ16sQIII", h, 0, b"EFI PART", 0x10000, 92, 0, 0,
                         my, alt, first_usable, last_usable, uuid.uuid4().bytes_le,
                         entries_lba, n_entries, esize, zlib.crc32(table))
        struct.pack_into("<I", h, 16, zlib.crc32(h))
        return bytes(h).ljust(BS, b"\0")

    with open(img, "wb") as f:
        f.truncate(total * BS)
        mbr = bytearray(512)
        struct.pack_into("<BBBBBBBBII", mbr, 446, 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF,
                         1, min(total - 1, 0xFFFFFFFF))
        mbr[510:512] = b"\x55\xaa"
        f.seek(0); f.write(mbr)
        f.seek(BS); f.write(header(1, total - 1, 2))
        f.seek(2 * BS); f.write(table)
        f.seek((total - 1 - entries_blocks) * BS); f.write(table)
        f.seek((total - 1) * BS); f.write(header(total - 1, 1, total - 1 - entries_blocks))
        for start, src in copies:
            with open(src, "rb") as s:
                f.seek(start * BS)
                f.write(s.read())


if __name__ == "__main__":
    main()
