#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate the SSDT the power-management lane boots with.

QEMU's own firmware declares no P-states, no battery, no AC adapter and no
thermal zone -- it has none -- so the kernel code that reads them (M129's
cpufreq through ACPI `_PSS`, M134's `/sys/class/power_supply` and
`/sys/class/thermal`) had nothing to read and nothing to prove.  This writes a
supplementary table that declares them, which QEMU loads with `-acpitable` and
this kernel then finds through the XSDT like any other SSDT: the values below
are the FIRMWARE's, and every check in the lane compares what the kernel
publishes against what is declared here.

It is AML by hand because the host has no iasl, and it is a script rather than
a checked-in blob so that what the firmware says is readable and reviewable.

Byte encodings are ACPI 6.5 section 20 (AML) and 6.4.3.7 (the Generic Register
descriptor `_PCT` uses).
"""
import struct
import sys

# ── AML primitives ────────────────────────────────────────────────────────────

def pkg_length(body_len: int) -> bytes:
    """PkgLength, which counts its own bytes as well as the body's."""
    for n in range(0, 4):
        total = body_len + 1 + n
        if n == 0:
            if total < 0x40:
                return bytes([total])
            continue
        if total < (1 << (4 + 8 * n)):
            first = (n << 6) | (total & 0x0F)
            out = [first]
            rest = total >> 4
            for _ in range(n):
                out.append(rest & 0xFF)
                rest >>= 8
            return bytes(out)
    raise ValueError("package too long")


def name_seg(name: str) -> bytes:
    s = (name + "____")[:4].upper()
    if not (s[0].isalpha() or s[0] == "_"):
        raise ValueError("a NameSeg starts with a letter or an underscore")
    return s.encode("ascii")


def name_string(path: str) -> bytes:
    """An absolute (\\X.Y) or relative (X.Y) NameString."""
    out = b""
    if path.startswith("\\"):
        out += b"\x5c"
        path = path[1:]
    segs = [p for p in path.split(".") if p]
    if len(segs) == 0:
        return out + b"\x00"          # RootChar alone
    if len(segs) == 1:
        return out + name_seg(segs[0])
    if len(segs) == 2:
        return out + b"\x2e" + name_seg(segs[0]) + name_seg(segs[1])
    out += b"\x2f" + bytes([len(segs)])
    for s in segs:
        out += name_seg(s)
    return out


def integer(v: int) -> bytes:
    if v == 0:
        return b"\x00"
    if v == 1:
        return b"\x01"
    if v <= 0xFF:
        return b"\x0a" + bytes([v])
    if v <= 0xFFFF:
        return b"\x0b" + struct.pack("<H", v)
    if v <= 0xFFFFFFFF:
        return b"\x0c" + struct.pack("<I", v)
    return b"\x0e" + struct.pack("<Q", v)


def string(s: str) -> bytes:
    return b"\x0d" + s.encode("ascii") + b"\x00"


def eisa_id(s: str) -> int:
    """The packed 32-bit form of a seven-character EISA id, as AML stores it."""
    if len(s) != 7:
        raise ValueError("an EISA id is seven characters")
    mfg = (((ord(s[0]) - 0x40) & 0x1F) << 10 |
           ((ord(s[1]) - 0x40) & 0x1F) << 5 |
           ((ord(s[2]) - 0x40) & 0x1F))
    d0 = int(s[3:5], 16)
    d1 = int(s[5:7], 16)
    return ((mfg >> 8) & 0xFF) | ((mfg & 0xFF) << 8) | (d0 << 16) | (d1 << 24)


def buffer(data: bytes) -> bytes:
    body = integer(len(data)) + data
    return b"\x11" + pkg_length(len(body)) + body


def package(elements) -> bytes:
    body = bytes([len(elements)]) + b"".join(elements)
    return b"\x12" + pkg_length(len(body)) + body


def name(path: str, value: bytes) -> bytes:
    return b"\x08" + name_string(path) + value


def scope(path: str, body: bytes) -> bytes:
    inner = name_string(path) + body
    return b"\x10" + pkg_length(len(inner)) + inner


def device(path: str, body: bytes) -> bytes:
    inner = name_string(path) + body
    return b"\x5b\x82" + pkg_length(len(inner)) + inner


def thermal_zone(path: str, body: bytes) -> bytes:
    inner = name_string(path) + body
    return b"\x5b\x85" + pkg_length(len(inner)) + inner


def generic_register(space: int, bit_width: int, bit_offset: int,
                     access_size: int, address: int) -> bytes:
    """The Generic Register resource descriptor (ACPI 6.4.3.7), as `_PCT`
    holds it: a large resource item, tag 0x82, twelve bytes of body."""
    body = bytes([space, bit_width, bit_offset, access_size]) + \
        struct.pack("<Q", address)
    assert len(body) == 12
    return buffer(b"\x82" + struct.pack("<H", len(body)) + body)


# ── what this firmware declares ───────────────────────────────────────────────

SPACE_SYSTEM_IO = 0x01
# An I/O port nothing in a q35 machine claims: the write is accepted by the
# chipset and read back as 0xff, which is exactly what a platform that takes a
# P-state request without reporting one back looks like.
PCT_PORT = 0x0810

# frequency MHz, power mW, transition us, bus-master us, control, status
P_STATES = [
    (2400, 35000, 10, 10, 0x1800, 0x1800),
    (1800, 22000, 10, 10, 0x1200, 0x1200),
    (1200, 14000, 10, 10, 0x0C00, 0x0C00),
     (600,  8000, 10, 10, 0x0600, 0x0600),
]

BAT_DESIGN_CAPACITY = 5200        # mWh
BAT_LAST_FULL = 5000              # mWh
BAT_WARN = 500
BAT_LOW = 200
BAT_REMAINING = 4200              # mWh, what _BST reports
BAT_RATE = 1100                   # mW
BAT_VOLTAGE = 11100               # mV
TZ_TEMP_DK = 3182                 # tenths of a kelvin: 45.05 degrees C


def body() -> bytes:
    cpu = device("BFRQ",
                 name("_HID", string("B1NX0001")) +
                 name("_UID", integer(0)) +
                 name("_PCT", package([
                     generic_register(SPACE_SYSTEM_IO, 16, 0, 2, PCT_PORT),
                     generic_register(SPACE_SYSTEM_IO, 16, 0, 2, PCT_PORT + 2),
                 ])) +
                 name("_PSS", package([
                     package([integer(f), integer(p), integer(l), integer(b),
                              integer(c), integer(st)])
                     for (f, p, l, b, c, st) in P_STATES
                 ])))

    bat = device("BAT0",
                 name("_HID", integer(eisa_id("PNP0C0A"))) +
                 name("_UID", integer(0)) +
                 # Present, enabled, functioning: bit 4 is what the kernel tests.
                 name("_STA", integer(0x1F)) +
                 # _BIF: power unit (0 = mW), design capacity, last full,
                 # technology (1 = rechargeable), design voltage, warning and
                 # low capacities, granularities, then four strings.
                 name("_BIF", package([
                     integer(0),
                     integer(BAT_DESIGN_CAPACITY),
                     integer(BAT_LAST_FULL),
                     integer(1),
                     integer(BAT_VOLTAGE),
                     integer(BAT_WARN),
                     integer(BAT_LOW),
                     integer(10),
                     integer(10),
                     string("B1NIX-FIXTURE"),
                     string("0001"),
                     string("LION"),
                     string("b1nix"),
                 ])) +
                 # _BST: state (1 = discharging), present rate, remaining
                 # capacity, present voltage.
                 name("_BST", package([
                     integer(1),
                     integer(BAT_RATE),
                     integer(BAT_REMAINING),
                     integer(BAT_VOLTAGE),
                 ])))

    adp = device("ADP1",
                 name("_HID", string("ACPI0003")) +
                 name("_UID", integer(0)) +
                 name("_PSR", integer(1)))

    tz = thermal_zone("TZ1",
                      name("_TMP", integer(TZ_TEMP_DK)) +
                      name("_CRT", integer(3732)) +
                      name("_TC1", integer(2)) +
                      name("_TC2", integer(5)) +
                      name("_TSP", integer(100)))

    return scope("\\_SB_", cpu + bat + adp + tz)


def table(aml: bytes) -> bytes:
    """An SSDT around the AML: the 36-byte description header, with the
    checksum computed over the whole table."""
    length = 36 + len(aml)
    hdr = bytearray(b"SSDT")
    hdr += struct.pack("<I", length)
    hdr += bytes([2])                     # revision: 64-bit integers
    hdr += bytes([0])                     # checksum, filled below
    hdr += b"B1NIX "                      # OEMID, six bytes
    hdr += b"B1FIXTUR"                    # OEM table id, eight bytes
    hdr += struct.pack("<I", 1)           # OEM revision
    hdr += b"B1NX"                        # creator id
    hdr += struct.pack("<I", 1)           # creator revision
    assert len(hdr) == 36
    blob = bytes(hdr) + aml
    hdr[9] = (-sum(blob)) & 0xFF
    return bytes(hdr) + aml


def declarations() -> str:
    """The same numbers in shell form, so the checks compare the kernel's
    output against THIS file rather than against a copy of it."""
    khz = " ".join(str(f * 1000) for (f, *_rest) in P_STATES)
    ctrl = " ".join("0x%x" % c for (_f, _p, _l, _b, c, _s) in P_STATES)
    return (
        "# generated by tests/support/acpi/mk-ssdt.py -- what the fixture"
        " firmware declares\n"
        'ACPI_FIXTURE_PSS_KHZ="%s"\n' % khz +
        'ACPI_FIXTURE_PSS_CONTROL="%s"\n' % ctrl +
        "ACPI_FIXTURE_PSS_COUNT=%d\n" % len(P_STATES) +
        "ACPI_FIXTURE_PCT_PORT=0x%x\n" % PCT_PORT +
        "ACPI_FIXTURE_BAT_DESIGN_MWH=%d\n" % BAT_DESIGN_CAPACITY +
        "ACPI_FIXTURE_BAT_FULL_MWH=%d\n" % BAT_LAST_FULL +
        "ACPI_FIXTURE_BAT_NOW_MWH=%d\n" % BAT_REMAINING +
        "ACPI_FIXTURE_BAT_NOW_UWH=%d\n" % (BAT_REMAINING * 1000) +
        "ACPI_FIXTURE_BAT_FULL_UWH=%d\n" % (BAT_LAST_FULL * 1000) +
        "ACPI_FIXTURE_BAT_VOLTAGE_UV=%d\n" % (BAT_VOLTAGE * 1000) +
        "ACPI_FIXTURE_BAT_RATE_MW=%d\n" % BAT_RATE +
        "ACPI_FIXTURE_BAT_VOLTAGE_MV=%d\n" % BAT_VOLTAGE +
        "ACPI_FIXTURE_BAT_CAPACITY_PCT=%d\n"
        % ((BAT_REMAINING * 100) // BAT_LAST_FULL) +
        "ACPI_FIXTURE_TZ_TEMP_DK=%d\n" % TZ_TEMP_DK +
        # Tenths of a kelvin to millidegrees Celsius: absolute zero is
        # -273.15 degrees, so the offset is 2731.5 tenths, not 2732.
        "ACPI_FIXTURE_TZ_TEMP_MC=%d\n" % (TZ_TEMP_DK * 100 - 273150)
    )


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write("usage: mk-ssdt.py <out.aml>\n")
        return 2
    blob = table(body())
    with open(sys.argv[1], "wb") as f:
        f.write(blob)
    with open(sys.argv[1] + ".env", "w") as f:
        f.write(declarations())
    sys.stderr.write("mk-ssdt: %s, %d bytes, %d P-states\n"
                     % (sys.argv[1], len(blob), len(P_STATES)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
