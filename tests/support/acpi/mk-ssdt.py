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


def if_(predicate: bytes, body: bytes) -> bytes:
    inner = predicate + body
    return b"\xA0" + pkg_length(len(inner)) + inner


def lequal(a: bytes, b: bytes) -> bytes:
    return b"\x93" + a + b


def bmd_pkg(status: int) -> bytes:
    """_BMD: status, capabilities (charging can be disabled, discharge on
    AC), recalibrate count, quick and slow recalibrate times."""
    return package([integer(status), integer(0x6), integer(0), integer(0),
                    integer(0)])


def name(path: str, value: bytes) -> bytes:
    return b"\x08" + name_string(path) + value


def scope(path: str, body: bytes) -> bytes:
    inner = name_string(path) + body
    return b"\x10" + pkg_length(len(inner)) + inner


def device(path: str, body: bytes) -> bytes:
    inner = name_string(path) + body
    return b"\x5b\x82" + pkg_length(len(inner)) + inner


def method(path: str, nargs: int, body: bytes) -> bytes:
    inner = name_string(path) + bytes([nargs & 7]) + body
    return b"\x14" + pkg_length(len(inner)) + inner


def return_(value: bytes) -> bytes:
    return b"\xa4" + value


def store(src: bytes, dst: str) -> bytes:
    return b"\x70" + src + name_string(dst)


def power_resource(path: str, body: bytes) -> bytes:
    # System level 0, resource order 0.
    inner = name_string(path) + b"\x00" + b"\x00\x00" + body
    return b"\x5b\x84" + pkg_length(len(inner)) + inner


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

# _PPC: the fastest state the platform allows now -- index 1, so the ceiling
# the kernel honours is below what the processor can do, and a kernel that
# ignored it would be caught running the first state.
PPC = 1

# _CST: C1 by HLT, and two deeper states entered by reading an I/O port --
# ports nothing on the machine claims, so the read is harmless and the halt
# the kernel follows it with is what idles the guest. Type, latency us, power
# mW, and the register (None for C1's FFH halt).
SPACE_FFH = 0x7F
C_STATES = [
    (1, 1, 1000, None),
    (2, 20, 500, 0x0820),
    (3, 100, 100, 0x0821),
]

BAT_DESIGN_CAPACITY = 5200        # mWh
BAT_LAST_FULL = 5000              # mWh
BAT_WARN = 500
BAT_LOW = 200
BAT_REMAINING = 4200              # mWh, what _BST reports
BAT_RATE = 1100                   # mW
BAT_VOLTAGE = 11100               # mV
BAT_CYCLES = 42                   # _BIX's cycle count
TZ_TEMP_DK = 3182                 # tenths of a kelvin: 45.05 degrees C
# The zone's trips, in tenths of a kelvin, in the order Linux lists them:
# critical, hot, passive, then the active trips hottest first. _AC0 turns on
# both fans, _AC1 only the first.
TZ_CRT_DK = 3732
TZ_HOT_DK = 3682
TZ_PSV_DK = 3432
TZ_AC_DK = [3532, 3382]
TZ_AL = [["FAN0", "FAN1"], ["FAN0"]]
TZ_TSP = 5                        # tenths of a second between passive steps
FANS = ["FAN0", "FAN1"]


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
                 ])) +
                 name("_PPC", integer(PPC)) +
                 name("_CST", package(
                     [integer(len(C_STATES))] +
                     [package([
                         # FFH, vendor 1 (Intel), class 1: C1 by HLT.
                         generic_register(SPACE_FFH, 1, 1, 0, 0)
                         if port is None else
                         generic_register(SPACE_SYSTEM_IO, 8, 0, 1, port),
                         integer(t), integer(lat), integer(pw)])
                      for (t, lat, pw, port) in C_STATES])))

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
                 # _BIX (ACPI 4.0, revision 1): the same numbers after a
                 # revision field, plus the cycle count, measurement accuracy,
                 # sampling and averaging times, and the swapping capability.
                 name("_BIX", package([
                     integer(1),
                     integer(0),
                     integer(BAT_DESIGN_CAPACITY),
                     integer(BAT_LAST_FULL),
                     integer(1),
                     integer(BAT_VOLTAGE),
                     integer(BAT_WARN),
                     integer(BAT_LOW),
                     integer(BAT_CYCLES),
                     integer(95000),
                     integer(0xFFFFFFFF),
                     integer(0xFFFFFFFF),
                     integer(1000),
                     integer(500),
                     integer(10),
                     integer(10),
                     string("B1NIX-FIXTURE"),
                     string("0001"),
                     string("LION"),
                     string("b1nix"),
                     integer(0),
                 ])) +
                 # _BTP: the trip the firmware notifies at, kept where the
                 # rest of the battery's AML could read it.
                 name("BTPV", integer(0)) +
                 method("_BTP", 1, store(b"\x68", "BTPV")) +
                 # _BMC keeps what it was asked; _BMD reports it back as the
                 # status (element 0) next to what the battery can do
                 # (element 1: charging can be disabled, and it can be
                 # discharged on AC).
                 name("BMCV", integer(0)) +
                 method("_BMC", 1, store(b"\x68", "BMCV")) +
                 method("_BMD", 0,
                        b"".join(if_(lequal(name_string("BMCV"), integer(v)),
                                     return_(bmd_pkg(v))) for v in (2, 4)) +
                        return_(bmd_pkg(0))) +
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

    # A fan is switched by the power resource in its _PR0; the resource's
    # _STA reads back what its _ON and _OFF stored.
    fans = b""
    for i, fan in enumerate(FANS):
        pr = "PFN%d" % i
        st = "FST%d" % i
        fans += power_resource(pr,
                               name(st, integer(0)) +
                               method("_STA", 0, return_(name_string(st))) +
                               method("_ON_", 0, store(integer(1), st)) +
                               method("_OFF", 0, store(integer(0), st)))
        fans += device(fan,
                       name("_HID", integer(eisa_id("PNP0C0B"))) +
                       name("_UID", integer(i)) +
                       name("_PR0", package([name_string("\\_SB_." + pr)])))

    trips = (name("_CRT", integer(TZ_CRT_DK)) +
             name("_HOT", integer(TZ_HOT_DK)) +
             name("_PSV", integer(TZ_PSV_DK)) +
             name("_PSL", package([name_string("\\_SB_.BFRQ")])))
    for i, (ac, al) in enumerate(zip(TZ_AC_DK, TZ_AL)):
        trips += name("_AC%d" % i, integer(ac))
        trips += name("_AL%d" % i,
                      package([name_string("\\_SB_." + f) for f in al]))

    tz = thermal_zone("TZ1",
                      name("_TMP", integer(TZ_TEMP_DK)) +
                      trips +
                      name("_TC1", integer(2)) +
                      name("_TC2", integer(5)) +
                      name("_TSP", integer(TZ_TSP)))

    return scope("\\_SB_", cpu + bat + adp + fans + tz)


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
        "ACPI_FIXTURE_PPC=%d\n" % PPC +
        'ACPI_FIXTURE_CST="%s"\n' % " ".join(
            "C%d:%d" % (t, lat) for (t, lat, _pw, _port) in C_STATES) +
        "ACPI_FIXTURE_PPC_KHZ=%d\n" % (P_STATES[PPC][0] * 1000) +
        "ACPI_FIXTURE_PCT_PORT=0x%x\n" % PCT_PORT +
        "ACPI_FIXTURE_BAT_DESIGN_MWH=%d\n" % BAT_DESIGN_CAPACITY +
        "ACPI_FIXTURE_BAT_FULL_MWH=%d\n" % BAT_LAST_FULL +
        "ACPI_FIXTURE_BAT_NOW_MWH=%d\n" % BAT_REMAINING +
        "ACPI_FIXTURE_BAT_NOW_UWH=%d\n" % (BAT_REMAINING * 1000) +
        "ACPI_FIXTURE_BAT_FULL_UWH=%d\n" % (BAT_LAST_FULL * 1000) +
        "ACPI_FIXTURE_BAT_VOLTAGE_UV=%d\n" % (BAT_VOLTAGE * 1000) +
        "ACPI_FIXTURE_BAT_RATE_MW=%d\n" % BAT_RATE +
        'ACPI_FIXTURE_BAT_INFO="cycles %d design %d vmin %d tech Li-ion '
        'model B1NIX-FIXTURE serial 0001 maker b1nix"\n'
        % (BAT_CYCLES, BAT_DESIGN_CAPACITY * 1000, BAT_VOLTAGE * 1000) +
        "ACPI_FIXTURE_BAT_VOLTAGE_MV=%d\n" % BAT_VOLTAGE +
        "ACPI_FIXTURE_BAT_CAPACITY_PCT=%d\n"
        % ((BAT_REMAINING * 100) // BAT_LAST_FULL) +
        "ACPI_FIXTURE_TZ_TEMP_DK=%d\n" % TZ_TEMP_DK +
        'ACPI_FIXTURE_TZ_TRIPS="%s"\n' % " ".join(
            "%s:%d" % (kind, dk * 100 - 273150) for (kind, dk) in
            [("critical", TZ_CRT_DK), ("hot", TZ_HOT_DK),
             ("passive", TZ_PSV_DK)] + [("active", a) for a in TZ_AC_DK]) +
        'ACPI_FIXTURE_TZ_CDEVS="Processor %s"\n' % " ".join(
            "Fan" for _ in FANS) +
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
