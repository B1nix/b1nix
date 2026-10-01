#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Do to a guest what only the outside of the machine can: move a tablet after
a sleep, press its power button, plug a card into it.

m135_acpi_smoke asks for each on the serial line; this watches the lane's log
for the request and answers over QMP -- `system_powerdown` is QEMU's power
button (the PM1 fixed event), and `device_add` of a PCI function is ACPI PCI
hotplug: QEMU raises GPE 1, whose method notifies the slot.

usage: acpi-actor.py <qmp-socket> <serial-log> [timeout_seconds]
"""
import json
import socket
import sys
import time

ACTIONS = [
    # After the S3 in m129_suspend_smoke: input on a device the sleep reset,
    # to prove its driver rebuilt it (M135).
    # No "device": naming one makes QEMU 11.1 abort looking for it among the
    # consoles; unnamed, the event goes to the active absolute pointer, which
    # is one of the two virtio-input devices either way.
    ("M129-SUSPEND: move the tablet",
     {"execute": "input-send-event",
      "arguments": {"events": [{"type": "abs",
                                "data": {"axis": "x", "value": 12345}},
                               {"type": "abs",
                                "data": {"axis": "y", "value": 23456}}]}}),
    ("M135-ACPI: press the power button", {"execute": "system_powerdown"}),
    ("M135-ACPI: plug a pci device",
     {"execute": "device_add",
      "arguments": {"driver": "virtio-rng-pci", "id": "hprng"}}),
]


def log(msg: str) -> None:
    sys.stderr.write("ACPI-ACTOR: %s\n" % msg)
    sys.stderr.flush()


def qmp(sock_path: str, command: dict, timeout: float) -> bool:
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
            f.readline()
            f.write(b'{"execute":"qmp_capabilities"}\n')
            f.flush()
            f.readline()
            f.write(json.dumps(command).encode() + b"\n")
            f.flush()
            # Asynchronous events may come first; the reply has "return" or
            # "error".
            for _ in range(20):
                reply = f.readline().decode(errors="replace").strip()
                if '"return"' in reply or '"error"' in reply:
                    log("%s -> %s" % (command["execute"], reply))
                    return '"error"' not in reply
            return False
        except OSError as e:
            log("%s failed: %s" % (command["execute"], e))
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
    timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 600.0
    deadline = time.time() + timeout
    done = 0
    offset = 0
    while done < len(ACTIONS) and time.time() < deadline:
        try:
            with open(log_path, "rb") as f:
                data = f.read()
        except FileNotFoundError:
            data = b""
        marker, command = ACTIONS[done]
        at = data.find(marker.encode(), offset)
        if at >= 0:
            offset = at + len(marker)
            time.sleep(1.0)  # the guest is reading by now
            qmp(sock_path, command, 30.0)
            done += 1
            continue
        time.sleep(0.2)
    if done < len(ACTIONS):
        log("the guest asked for %d of %d actions" % (done, len(ACTIONS)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
