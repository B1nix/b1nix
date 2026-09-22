#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Wake a guest that has put itself into ACPI S3.

An S3 sleep powers the processor off, so nothing inside the guest can end it:
on real hardware the chipset does, on a signal it was armed for (an RTC alarm, a
power button, a wake-on-LAN packet). QEMU models the sleep faithfully — the vCPUs
stop and the machine is "suspended" — but its RTC does not drive the ACPI wake
path, so the wake has to come from outside, and `system_wakeup` over QMP is that
outside: the same thing a power button is.

Watches the lane's serial log for the kernel's own "entering S3" line, waits the
interval the guest armed its alarm for, and then wakes it. Prints one line per
step, which lands in the lane's output, so a run that never woke says which half
failed.

usage: s3-waker.py <qmp-socket> <serial-log> [delay_seconds] [timeout_seconds]
"""
import json
import os
import socket
import sys
import time

MARKER = "power: mem (S3)"


def log(msg: str) -> None:
    sys.stderr.write("S3-WAKER: %s\n" % msg)
    sys.stderr.flush()


def wait_for_marker(path: str, timeout: float) -> bool:
    deadline = time.time() + timeout
    seen = 0
    while time.time() < deadline:
        try:
            with open(path, "rb") as f:
                data = f.read()
        except FileNotFoundError:
            data = b""
        if MARKER.encode() in data[seen:]:
            return True
        seen = max(0, len(data) - len(MARKER))
        time.sleep(0.2)
    return False


def qmp(sock_path: str, command: str, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(5.0)
            s.connect(sock_path)
        except OSError:
            time.sleep(0.2)
            continue
        try:
            f = s.makefile("rwb")
            f.readline()                      # the greeting
            f.write(b'{"execute":"qmp_capabilities"}\n')
            f.flush()
            f.readline()
            f.write(json.dumps({"execute": command}).encode() + b"\n")
            f.flush()
            reply = f.readline().decode(errors="replace").strip()
            log("%s -> %s" % (command, reply))
            if "not in suspended state" in reply:
                # The guest woke on its own -- QEMU's RTC does drive the wake
                # path after all, so the alarm the guest armed was enough. That
                # is the better outcome: nothing outside the machine was needed.
                log("guest had already woken by itself")
                return True
            return '"error"' not in reply
        except OSError as e:
            log("%s failed: %s" % (command, e))
            return False
        finally:
            s.close()
    log("no QMP socket at %s" % sock_path)
    return False


def main() -> int:
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__ or "")
        return 2
    sock_path, log_path = sys.argv[1], sys.argv[2]
    delay = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0
    timeout = float(sys.argv[4]) if len(sys.argv) > 4 else 300.0

    if not wait_for_marker(log_path, timeout):
        log("the guest never announced an S3 (nothing to wake)")
        return 0
    log("guest is in S3; waking it in %.1f s" % delay)
    time.sleep(delay)
    ok = qmp(sock_path, "system_wakeup", 30.0)
    log("wake %s" % ("sent" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
