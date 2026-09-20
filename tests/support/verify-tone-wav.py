#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Is there a tone in what the emulator played?

QEMU's wav audio backend writes everything a guest plays to a file, so a run
that plays a sine leaves the samples on the host — which is the only way to
check the sound path end to end without listening to it. The driver's own
markers say the controller fetched the buffer; this says what came out.

  verify-tone-wav.py FILE [EXPECTED_HZ]

Prints one line and exits 0 when the file holds a tone near EXPECTED_HZ
(default 440), non-zero otherwise. The header's length fields are ignored:
QEMU is killed rather than shut down, so it never writes them back.
"""
import struct
import sys
import math


def main() -> int:
    if len(sys.argv) < 2:
        print("AUDIO-WAV: fail usage")
        return 2
    path = sys.argv[1]
    want = float(sys.argv[2]) if len(sys.argv) > 2 else 440.0

    try:
        raw = open(path, "rb").read()
    except OSError as e:
        print(f"AUDIO-WAV: fail cannot read {path}: {e}")
        return 1
    if len(raw) < 64 or raw[:4] != b"RIFF":
        print(f"AUDIO-WAV: fail not a wav file ({len(raw)} bytes)")
        return 1

    # Walk the chunks to the format and the data, rather than assuming the
    # canonical 44-byte header: QEMU's is canonical today and need not stay so.
    pos, rate, channels, bits, data = 12, 0, 0, 0, b""
    while pos + 8 <= len(raw):
        cid = raw[pos:pos + 4]
        (size,) = struct.unpack("<I", raw[pos + 4:pos + 8])
        body = raw[pos + 8:pos + 8 + size] if size else raw[pos + 8:]
        if cid == b"fmt ":
            _, channels, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            # The size field is written when the file is closed, which never
            # happens here; take the rest of the file.
            data = raw[pos + 8:]
            break
        pos += 8 + size + (size & 1)

    if bits != 16 or channels == 0 or rate == 0:
        print(f"AUDIO-WAV: fail unsupported format rate={rate} ch={channels} bits={bits}")
        return 1

    frames = len(data) // (2 * channels)
    if frames == 0:
        print("AUDIO-WAV: fail no samples")
        return 1
    mono = struct.unpack(f"<{frames * channels}h", data[:frames * channels * 2])[::channels]

    # Goertzel: the power a single frequency leaves in a window. A tone leaves
    # it and noise does not, and it is cheap enough to scan a spectrum with.
    def goertzel(chunk, freq: float) -> float:
        k = 2.0 * math.cos(2.0 * math.pi * freq / rate)
        s1 = s2 = 0.0
        for v in chunk:
            s0 = v + k * s1 - s2
            s2, s1 = s1, s0
        return s1 * s1 + s2 * s2 - k * s1 * s2

    # The window where the WANTED frequency is strongest, not the loudest one:
    # the machine has two sound devices playing into the same capture, and the
    # loudest tenth of a second is where they overlap and beat against each
    # other, which reads as a pitch neither of them played.
    win = max(1, int(rate * 0.1))
    best_p, best_at = -1.0, 0
    for start in range(0, max(1, len(mono) - win), max(1, win // 2)):
        p = goertzel(mono[start:start + win], want)
        if p > best_p:
            best_p, best_at = p, start
    chunk = mono[best_at:best_at + win]
    best_rms = math.sqrt(sum(float(v) * v for v in chunk) / len(chunk))
    if best_rms < 200.0:
        print(f"AUDIO-WAV: fail silence (rms {best_rms:.0f} over {frames} frames)")
        return 1

    # The dominant frequency of that window has to be the wanted one — a
    # strong 440 in a window whose peak is elsewhere is a harmonic or a beat,
    # not the tone.
    peak_f, peak_p = 0.0, -1.0
    f = 100.0
    while f <= 2000.0:
        p = goertzel(chunk, f)
        if p > peak_p:
            peak_f, peak_p = f, p
        f += 5.0
    # Refine around the peak.
    f = peak_f - 5.0
    while f <= peak_f + 5.0:
        p = goertzel(chunk, f)
        if p > peak_p:
            peak_f, peak_p = f, p
        f += 0.5
    freq = peak_f
    ok = abs(freq - want) <= want * 0.10
    print(
        f"AUDIO-WAV: {'ok' if ok else 'fail'} tone freq={freq:.0f}Hz "
        f"want={want:.0f}Hz rms={best_rms:.0f} frames={frames} rate={rate}"
    )
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
