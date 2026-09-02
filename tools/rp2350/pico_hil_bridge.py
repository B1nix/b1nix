# ==============================================================================
# RP2350 (Raspberry Pi Pico 2) — HIL UART Bridge & Multi-State LED Controller
# ==============================================================================
# MicroPython script for RP2350 (Pico 2) or RP2040 (Pico).
#
# LED Indication States:
#   1. Standby (Pico ON, waiting for USB): Slow Heartbeat Pulse (1 flash / 1.5s)
#   2. USB Connected (Terminal Open):       Solid ON (Steady glow)
#   3. Active UART RX/TX Data Traffic:     Rapid Activity Flicker (Strobe)
#   4. Target Board Powered OFF:           Double-Blink Warning Pulse
#   5. Target Hardware Reset:              Fast 25Hz Strobe during reset
#
# Pinout:
#   RP2350 GP0 (UART0 TX)  -->  Raspberry Pi 4 Pin 10 (GPIO 15 / RXD)
#   RP2350 GP1 (UART0 RX)  -->  Raspberry Pi 4 Pin 8  (GPIO 14 / TXD)
#   RP2350 GP22 (Reset)    -->  Raspberry Pi 4 GLOBAL_EN / RUN (Pin 39)
#   RP2350 GP21 (PwrSense) -->  Raspberry Pi 4 3.3V Rail (Pin 1)
#   RP2350 GND             -->  Raspberry Pi 4 Pin 6  (GND)
# ==============================================================================

import machine
import select
import sys
import time

# Pin configuration
UART_ID = 0
BAUD_RATE = 115200
TX_PIN = machine.Pin(0)
RX_PIN = machine.Pin(1)
RESET_PIN = machine.Pin(10, machine.Pin.OUT, value=1)  # Active LOW
POWER_SENSE_PIN = machine.Pin(11, machine.Pin.IN, machine.Pin.PULL_DOWN)

# Onboard LED (GP25 or board LED)
try:
    LED = machine.Pin("LED", machine.Pin.OUT)
except Exception:
    LED = machine.Pin(25, machine.Pin.OUT)

uart = machine.UART(UART_ID, baudrate=BAUD_RATE, tx=TX_PIN, rx=RX_PIN, rxbuf=4096, timeout=5)

# Status variables
traffic_active_until = 0
reset_pulse_until = 0
usb_connected = False
cmd_buf = b""
CMD_RESET = b"@@RESET@@"

poll = select.poll()
poll.register(sys.stdin, select.POLLIN)

def trigger_hardware_reset():
    global reset_pulse_until
    RESET_PIN.value(0) # Pull LOW
    reset_pulse_until = time.ticks_add(time.ticks_ms(), 100)
    sys.stdout.buffer.write(b"\r\n[RP2350: Hardware Reset Triggered]\r\n")

while True:
    now = time.ticks_ms()

    # 1. Update Hardware Reset state
    if reset_pulse_until != 0:
        if time.ticks_diff(now, reset_pulse_until) >= 0:
            RESET_PIN.value(1) # Release HIGH
            reset_pulse_until = 0

    # 2. Forward UART (Target -> Host)
    if uart.any():
        data = uart.read()
        if data:
            sys.stdout.buffer.write(data)
            traffic_active_until = time.ticks_add(now, 100)

    # 3. Forward USB stdin (Host -> Target)
    events = poll.poll(0)
    if events:
        usb_connected = True
        host_data = sys.stdin.buffer.read(64)
        if host_data:
            traffic_active_until = time.ticks_add(now, 100)
            cmd_buf += host_data
            if CMD_RESET in cmd_buf:
                trigger_hardware_reset()
                cmd_buf = b""
            else:
                if len(cmd_buf) > 32:
                    cmd_buf = cmd_buf[-32:]
                uart.write(host_data)

    # 4. LED State Machine
    if reset_pulse_until != 0:
        # State: Hardware Reset in progress -> Fast 25Hz strobe
        LED.value(1 if (now % 40) < 20 else 0)
    elif time.ticks_diff(traffic_active_until, now) > 0:
        # State: Active UART Data Streaming -> Activity Flicker
        LED.value(1 if (now % 60) < 30 else 0)
    elif usb_connected:
        target_on = POWER_SENSE_PIN.value()
        # State: USB Open, but Target 3.3V is off -> Double flash warning
        if not target_on:
            phase = now % 1000
            LED.value(1 if (phase < 60 or (phase >= 160 and phase < 220)) else 0)
        else:
            # State: USB Connected & Target Normal -> Solid ON
            LED.value(1)
    else:
        # State: Standby (Powered, waiting for USB) -> Slow Heartbeat
        phase = now % 1500
        LED.value(1 if phase < 60 else 0)
