#!/usr/bin/env python3
"""Write a GPT, by default with 4 KiB logical blocks — the layout of a phone's
UFS LUN. A PC disk wants 512-byte blocks, so the block size is an option.

    mkgpt4k.py [--block-size N] IMAGE SIZE_MIB SPEC [SPEC ...]

    SPEC = NAME:SIZE_MIB[:FILE[:TYPE]]

Partitions are laid out in order from the first usable block. A FILE, when
given, is copied to the start of its partition (an ext4 image made by mke2fs,
or a FAT image made by mformat, for instance). TYPE is "linux" (the
default), "esp" (what UEFI firmware looks for) or "biosboot".

Protective MBR, primary header at block 1, entries at block 2, backup at the
end.
"""
import struct
import sys
import uuid
import zlib

TYPES = {
    "linux": uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4"),
    "esp": uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b"),
    # Where a BIOS bootloader puts the stage that does not fit in the MBR gap.
    # Limine refuses to install to a GPT disk without one.
    "biosboot": uuid.UUID("21686148-6449-6e6f-744e-656564454649"),
}


def main():
    argv = sys.argv[1:]
    bs = 4096
    while argv and argv[0].startswith("--"):
        if argv[0] == "--block-size":
            bs = int(argv[1])
            argv = argv[2:]
        else:
            sys.exit(f"unknown option {argv[0]}")
    if len(argv) < 3:
        sys.exit(__doc__)

    img, size_mib, specs = argv[0], int(argv[1]), argv[2:]
    total = size_mib * 1024 * 1024 // bs
    n_entries, esize = 128, 128
    entries_blocks = (n_entries * esize + bs - 1) // bs
    first_usable = 2 + entries_blocks
    last_usable = total - 2 - entries_blocks

    table = bytearray(n_entries * esize)
    lba = first_usable
    copies = []
    for i, spec in enumerate(specs):
        name, mib, *rest = spec.split(":")
        src = rest[0] if rest else ""
        kind = rest[1] if len(rest) > 1 else "linux"
        if kind not in TYPES:
            sys.exit(f"unknown partition type '{kind}'")
        blocks = int(mib) * 1024 * 1024 // bs
        if lba + blocks - 1 > last_usable:
            sys.exit(f"{name} does not fit")
        uname = name.encode("utf-16-le")[:72]
        struct.pack_into("<16s16sQQQ72s", table, i * esize, TYPES[kind].bytes_le,
                         uuid.uuid4().bytes_le, lba, lba + blocks - 1, 0, uname)
        if src:
            copies.append((lba, src))
        lba += blocks

    def header(my, alt, entries_lba):
        h = bytearray(92)
        struct.pack_into("<8sIIIIQQQQ16sQIII", h, 0, b"EFI PART", 0x10000, 92, 0, 0,
                         my, alt, first_usable, last_usable, uuid.uuid4().bytes_le,
                         entries_lba, n_entries, esize, zlib.crc32(table))
        struct.pack_into("<I", h, 16, zlib.crc32(h))
        return bytes(h).ljust(bs, b"\0")

    with open(img, "wb") as f:
        f.truncate(total * bs)
        mbr = bytearray(512)
        struct.pack_into("<BBBBBBBBII", mbr, 446, 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF,
                         1, min(total - 1, 0xFFFFFFFF))
        mbr[510:512] = b"\x55\xaa"
        f.seek(0); f.write(mbr)
        f.seek(bs); f.write(header(1, total - 1, 2))
        f.seek(2 * bs); f.write(table)
        f.seek((total - 1 - entries_blocks) * bs); f.write(table)
        f.seek((total - 1) * bs); f.write(header(total - 1, 1, total - 1 - entries_blocks))
        for start, src in copies:
            with open(src, "rb") as s:
                f.seek(start * bs)
                while True:
                    chunk = s.read(8 << 20)
                    if not chunk:
                        break
                    f.write(chunk)


if __name__ == "__main__":
    main()
