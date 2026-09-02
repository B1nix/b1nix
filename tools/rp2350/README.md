# RP2350 (Raspberry Pi Pico 2) & RP2040 HIL UART Bridge

This directory contains firmware based on **`Noltari/pico-uart-bridge`** with added **Multi-State LED Status Indication**, **Dual UART support**, **Target Power Sense**, and **Hardware Reset Control** for Raspberry Pi 4 and other test boards.

---

## 1. Multi-State LED Indication (Visual Status)

The onboard LED provides clear, real-time feedback on board and connection status:

| LED Pattern | Meaning | Description |
|---|---|---|
| **Slow Heartbeat** (1 flash / 1.5s) | **Power ON / Standby** | Pico 2 is powered on and alive, waiting for host PC to open serial port. |
| **Solid ON** (Steady Glow) | **USB Terminal Connected** | Host PC has connected to `/dev/cu.usbmodem*` (macOS) or `/dev/ttyACM*` (Linux). |
| **Fast Activity Flicker** (15Hz Strobe) | **Active UART Traffic** | Target board is actively streaming data (e.g. `b1nix` kernel boot logs or test markers). |
| **Double-Blink Warning** (2 flashes / 1s) | **Target Power OFF** | Host terminal is open, but Target 3.3V rail on GP21 is not detected (target is unpowered). |
| **Fast Strobe** (25Hz) | **Hardware Reset Active** | 100ms hardware reset pulse sent to target `GLOBAL_EN` / `RUN` pin. |

---

## 2. Hardware Wiring Table (Raspberry Pi 4)

Connect the Pico 2 (RP2350) to the Raspberry Pi 4 GPIO header:

| RP2350 Pin | Raspberry Pi 4 Pin | Function | Notes |
|---|---|---|---|
| **GP0** (Pin 1) | **Pin 10** (GPIO 15 / RXD) | Primary UART0 TX | Transmit to RPi 4 |
| **GP1** (Pin 2) | **Pin 8** (GPIO 14 / TXD) | Primary UART0 RX | Receive from RPi 4 |
| **GP10** | **Pin 39** (or `GLOBAL_EN`) | Target Hardware Reset | Active LOW reset pulse |
| **GP11** | **Pin 1** (3.3V Rail) | Target Power Sense | Senses target power status |
| **GND** | **Pin 6** (GND) | Ground Reference | Common ground |


*Secondary UART (UART1) is also available on **GP4** (TX) and **GP5** (RX).*

---

## 3. Quick Setup Options

### Option A: MicroPython (Drag & Drop, No Compilation)
1. Flash official **MicroPython for Pico 2 (RP2350)** `.uf2` onto your board.
2. Copy [`pico_hil_bridge.py`](file:///Users/dmytrom/Documents/GitHub/b1nix/tools/rp2350/pico_hil_bridge.py) onto the board as `main.py`.
3. Done! Connect USB to your PC.

### Option B: Build C SDK Firmware
```bash
cd tools/rp2350
PICO_BOARD=pico2 ./build.sh
```
Copy `build-pico2/b1nix_uart_bridge.uf2` to the Pico 2 in `BOOTSEL` mode.

---

## 4. Automated Testing with `hil.sh`

Run the one-click test runner:
```bash
# Full test cycle with automatic target hardware reset:
sh tools/rpi4/hil.sh --reset
```
