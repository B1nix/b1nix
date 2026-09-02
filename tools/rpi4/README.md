# Raspberry Pi 4 Network Boot (PXE/TFTP) & UART Automation Guide

This directory contains automated Hardware-in-the-Loop (HIL) tooling to build, deploy, boot over network (PXE/TFTP), monitor via UART, and report test results from a physical **Raspberry Pi 4 Model B**.

---

## 1. Hardware Setup (UART Console)

Connect a 3.3V USB-to-UART serial adapter (e.g. CP2102, FT232RL, CH340) between your host/CI server and the Raspberry Pi 4 GPIO header:

| USB-UART Pin | Raspberry Pi 4 Pin | Function |
|---|---|---|
| **GND** | **Pin 6** (GND) | Ground Reference |
| **RX** | **Pin 8** (GPIO 14 / TXD) | Serial Receive (Host reads RPi output) |
| **TX** | **Pin 10** (GPIO 15 / RXD) | Serial Transmit (Host sends to RPi) |

Baud rate: **115200 8N1**.

---

## 2. Raspberry Pi 4 EEPROM Configuration (One-Time Setup)

To enable native network boot on the Pi 4:

1. Insert an SD card with Raspberry Pi OS.
2. Run `sudo rpi-eeprom-config -e` to edit the bootloader configuration.
3. Set `BOOT_ORDER=0xf21` (Try network/TFTP first, fall back to SD card).
4. Save and reboot once so the EEPROM updates.

---

## 3. Server Configuration (`hil.conf`)

Edit `tools/rpi4/hil.conf` to set your paths and webhook URL:

```sh
# TFTP root directory where the Pi 4 fetches kernel and firmware over network (PXE)
TFTP_ROOT="/srv/tftp/rpi4"

# Serial port device for UART console (leave empty for automatic detection)
SERIAL_PORT=""

# Serial Baud Rate
BAUD_RATE=115200

# Maximum test timeout in seconds
TIMEOUT=120

# Optional Webhook URL to send JSON test report and full log
WEBHOOK_URL="https://your-server.com/api/test-results"
```

---

## 4. Running Automated Tests (`hil.sh`)

### Full Cycle (Build → Deploy TFTP → Listen UART → Report):
```bash
sh tools/rpi4/hil.sh
```

### Individual Subcommands:
- **Setup TFTP environment**:
  ```bash
  sh tools/rpi4/hil.sh setup
  ```
- **Build & Deploy kernel image only**:
  ```bash
  sh tools/rpi4/hil.sh deploy
  ```
- **Monitor UART console only**:
  ```bash
  sh tools/rpi4/hil.sh uart
  ```

---

## 5. Webhook JSON Payload Schema

When `WEBHOOK_URL` (or `--webhook <URL>`) is configured, `hil.sh` sends an HTTP POST with `Content-Type: application/json`:

```json
{
  "platform": "Raspberry Pi 4",
  "arch": "aarch64",
  "status": "PASS",
  "passed": true,
  "panicked": false,
  "duration_seconds": 12.45,
  "timestamp": "20260821-163000",
  "log_content": "b1nix kernel starting...\n...\nB1NIX-TEST: done\n"
}
```
