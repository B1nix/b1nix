#!/usr/bin/env python3
"""
B1NIX Sony Xperia 5 (SM8150 / Bahamut) Android Boot Image v2 Packer
Combines AArch64 Image + SM8150 Device Tree Blob into bootable Fastboot image.
"""
import sys
import os
import struct
import shutil
import subprocess
import tempfile

def align(size, page_size=4096):
    return ((size + page_size - 1) // page_size) * page_size

def check_dtb(data, path):
    """The bootloader reads the FIRST blob in this field. A file that is several
    device trees concatenated - the form qcom kernel builds ship them in - packs
    without complaint and boots the phone with whichever SoC happens to be first
    in the bundle, which is how an APQ8016 tree ended up describing an SM8150."""
    if data[:4] != b"\xd0\x0d\xfe\xed":
        raise SystemExit(f"[!] {path}: not a device tree blob")
    total = struct.unpack_from(">I", data, 4)[0]
    if total != len(data):
        raise SystemExit(
            f"[!] {path}: {len(data)} bytes but the first blob is only {total} - "
            "this is a bundle of several device trees. Split it and pass the "
            "SM8150 one."
        )


# What PID 1 and the kernel get told. This has to go into the DEVICE TREE, not
# just the boot header: the SM8150 tree ships its own /chosen/bootargs (a Linux
# string about RCU and cgroups) and the kernel takes the tree's copy over its
# compiled-in default — so a cmdline set anywhere else is silently ignored.
# /sbin/init, BusyBox, musl and the test binaries live in the ext4 rootfs, which
# this board reaches as the ram0 block device — the ramdisk slot of this very
# boot image. UFS is unsupported, so that ramdisk is the whole filesystem.
# Set by the Makefile through $BAHAMUT_BOOTARGS; the default here matches
# BAHAMUT_CMDLINE. Note that on this device
# the COMPILED-IN default is what actually reaches the kernel — the bootloader
# hands over a tree with no /chosen/bootargs and ignores the header cmdline —
# so this is belt and braces, not the control knob.
BOOTARGS = os.environ.get("BAHAMUT_BOOTARGS", "b1nix.loglevel=6")


def dtb_with_bootargs(dtb_path, bootargs):
    """Return a path to a DTB whose /chosen/bootargs is `bootargs`."""
    if not shutil.which("fdtput"):
        print("[!] fdtput not found — packing the tree's own bootargs unchanged.")
        print("    Install dtc (brew install dtc) to control the kernel cmdline.")
        return dtb_path
    tmp = tempfile.NamedTemporaryFile(suffix=".dtb", delete=False)
    tmp.close()
    shutil.copyfile(dtb_path, tmp.name)
    subprocess.run(["fdtput", "-t", "s", tmp.name, "/chosen", "bootargs", bootargs],
                   check=True)
    print(f"[*] bootargs = {bootargs!r}")
    return tmp.name


def pack_boot_img(kernel_path, dtb_path, output_path, ramdisk_path=None):
    dtb_path = dtb_with_bootargs(dtb_path, BOOTARGS)
    with open(kernel_path, "rb") as f:
        kernel_data = f.read()

    with open(dtb_path, "rb") as f:
        dtb_data = f.read()
    check_dtb(dtb_data, dtb_path)

    ramdisk_data = b""
    if ramdisk_path and os.path.exists(ramdisk_path):
        with open(ramdisk_path, "rb") as f:
            ramdisk_data = f.read()
        print(f"[*] ramdisk {len(ramdisk_data) // (1024*1024)} MiB from {ramdisk_path}")
    else:
        print("[!] no ramdisk — the device will have no /bin/sh "
              "(run tools/sony-xperia-5/mkramdisk_bahamut.sh)")

    kernel_size = len(kernel_data)
    ramdisk_size = len(ramdisk_data)
    second_size = 0
    recovery_dtbo_size = 0
    dtb_size = len(dtb_data)

    kernel_addr = 0x00008000
    # 0x84000000 physical (DRAM base + this), NOT the stock 0x01000000.
    #
    # The stock Android offsets assume a ramdisk of a few megabytes. Ours is
    # tens, and at 0x81000000 it swallowed both the loadable-module region
    # (0x80c00000..0x81c00000) and the device tree the bootloader places at
    # tags/dtb offset 0x01f00000 — the kernel then parsed its own rootfs as an
    # FDT. This lands it in the large free hole above the kernel and below
    # hyp_mem (0x85700000), clear of every carveout and of everything the
    # kernel places itself.
    ramdisk_addr = 0x02000000
    second_addr = 0x00000000
    tags_addr = 0x01E00000
    page_size = 4096
    header_version = 2
    os_version = 0

    name = b"b1nix-bahamut\x00".ljust(16, b"\x00")
    # The bootloader REPLACES /chosen/bootargs with this string, so this — not
    # the tree, and not the kernel's compiled-in default — is what PID 1 and
    # every b1nix.* option are actually read from. The androidboot.* parts are
    # kept because ABL looks at them on its own way through.
    cmdline = ("androidboot.hardware=qcom loop.max_part=16 " + BOOTARGS).encode()
    cmdline = cmdline.ljust(512, b"\x00")
    extra_cmdline = b"".ljust(1024, b"\x00")
    id_hash = b"\x00" * 32

    header_size = 1660
    recovery_dtbo_offset = 0
    dtb_addr = 0x01F00000

    header = bytearray(4096)
    header[0:8] = b"ANDROID!"
    struct.pack_into("<10I", header, 8, kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size, second_addr, tags_addr, page_size, header_version, os_version)
    header[48:64] = name
    header[64:576] = cmdline
    header[576:608] = id_hash
    header[608:1632] = extra_cmdline
    struct.pack_into("<QII", header, 1632, recovery_dtbo_size, recovery_dtbo_offset, header_size)
    struct.pack_into("<IQ", header, 1648, dtb_size, dtb_addr)

    with open(output_path, "wb") as out:
        out.write(header)
        out.write(kernel_data)
        out.write(b"\x00" * (align(kernel_size) - kernel_size))
        # The ramdisk sits between the kernel and the dtb in a v2 image, and a
        # zero-length section occupies no pages at all — which is why this used
        # to pack correctly with the field simply absent.
        out.write(ramdisk_data)
        out.write(b"\x00" * (align(ramdisk_size) - ramdisk_size))
        out.write(dtb_data)
        out.write(b"\x00" * (align(dtb_size) - dtb_size))

    print(f"[OK] Successfully packed {output_path} ({os.path.getsize(output_path)} bytes)")

if __name__ == "__main__":
    b1nix_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    default_kernel = os.path.join(b1nix_root, "build", "aarch64", "Image")
    default_dtb = os.path.join(b1nix_root, "tools", "dts", "sm8150-sony-bahamut.dtb")
    default_out = os.path.join(b1nix_root, "build", "aarch64", "b1nix_bahamut_boot.img")
    default_ramdisk = os.path.join(b1nix_root, "build", "aarch64", "bahamut-ramdisk.ext4")

    k = sys.argv[1] if len(sys.argv) > 1 else default_kernel
    d = sys.argv[2] if len(sys.argv) > 2 else default_dtb
    o = sys.argv[3] if len(sys.argv) > 3 else default_out

    os.makedirs(os.path.dirname(o), exist_ok=True)
    r = sys.argv[4] if len(sys.argv) > 4 else default_ramdisk
    pack_boot_img(k, d, o, r)
