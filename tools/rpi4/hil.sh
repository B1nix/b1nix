#!/bin/sh
set -eu

# ==============================================================================
# hil.sh — All-in-One Raspberry Pi 4 PXE Deploy & UART Test Automation
# ==============================================================================
# Usage:
#   sh tools/rpi4/hil.sh [run|setup|deploy|uart] [options]
#
# Options:
#   --config, -c <file>      Configuration file (default: tools/rpi4/hil.conf)
#   --tftp, -t <path>        TFTP root directory
#   --port, -p <dev>         Serial port (e.g. /dev/cu.usbserial* or /dev/ttyUSB0)
#   --baud, -b <rate>        Baud rate (default: 115200)
#   --timeout, -T <sec>      Test timeout in seconds (default: 120)
#   --webhook, -w <url>      Webhook URL to POST JSON test report
#   --help, -h               Show this help message
# ==============================================================================

PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
CONFIG_FILE="$PROJECT_DIR/tools/rpi4/hil.conf"

# Defaults
TFTP_ROOT="/srv/tftp/rpi4"
SERIAL_PORT=""
BAUD_RATE=115200
TIMEOUT=120
WEBHOOK_URL=""
KERNEL_CMDLINE="console=ttyAMA0,115200 b1nix.test=1 init=/bin/m12_smoke"

# Load config if present
if [ -f "$CONFIG_FILE" ]; then
    # shellcheck disable=SC1090
    . "$CONFIG_FILE"
fi

COMMAND="run"
if [ $# -gt 0 ]; then
    case "$1" in
        run|setup|deploy|uart)
            COMMAND="$1"
            shift
            ;;
        -*)
            COMMAND="run"
            ;;
        *)
            echo "Unknown command: $1"
            echo "Usage: $0 [run|setup|deploy|uart] [options]"
            exit 1
            ;;
    esac
fi

while [ $# -gt 0 ]; do
    case "$1" in
        --config|-c)
            CONFIG_FILE="$2"
            if [ -f "$CONFIG_FILE" ]; then
                . "$CONFIG_FILE"
            else
                echo "[-] Warning: Config file $CONFIG_FILE not found."
            fi
            shift 2
            ;;
        --tftp|-t)
            TFTP_ROOT="$2"
            shift 2
            ;;
        --port|-p)
            SERIAL_PORT="$2"
            shift 2
            ;;
        --baud|-b)
            BAUD_RATE="$2"
            shift 2
            ;;
        --timeout|-T)
            TIMEOUT="$2"
            shift 2
            ;;
        --webhook|-w)
            WEBHOOK_URL="$2"
            shift 2
            ;;
        --reset|-r)
            TRIGGER_RESET=1
            shift
            ;;
        --help|-h)
            echo "b1nix Raspberry Pi 4 HIL Test Suite"
            echo "Usage: $0 [run|setup|deploy|uart] [options]"
            echo "  run     : Full cycle (setup -> build & deploy -> listen UART -> report)"
            echo "  setup   : Prepare TFTP directory with firmware, config.txt, cmdline.txt"
            echo "  deploy  : Build kernel and copy Image + DTB to TFTP root"
            echo "  uart    : Listen to serial console and capture test logs"
            echo "  --reset : Send hardware reset trigger (@@RESET@@) to RP2350 bridge"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

auto_detect_port() {
    for p in /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.SLAB* /dev/cu.wchusbserial* /dev/ttyACM* /dev/ttyUSB* /dev/serial/by-id/*; do
        if [ -e "$p" ]; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

do_setup() {
    echo "================================================================"
    echo " [1/3] Preparing TFTP directory at: $TFTP_ROOT"
    echo "================================================================"
    mkdir -p "$TFTP_ROOT"

    # config.txt
    cat <<'CONFIG_EOF' > "$TFTP_ROOT/config.txt"
arm_64bit=1
kernel=kernel8.img
enable_uart=1
uart_2ndstage=1
dtoverlay=disable-bt
CONFIG_EOF
    echo "[+] Wrote $TFTP_ROOT/config.txt"

    # cmdline.txt
    echo "$KERNEL_CMDLINE" > "$TFTP_ROOT/cmdline.txt"
    echo "[+] Wrote $TFTP_ROOT/cmdline.txt ($KERNEL_CMDLINE)"

    # DTB
    if [ -f "$PROJECT_DIR/tools/dts/bcm2711-rpi-4-b.dtb" ]; then
        cp -f "$PROJECT_DIR/tools/dts/bcm2711-rpi-4-b.dtb" "$TFTP_ROOT/bcm2711-rpi-4-b.dtb"
        echo "[+] Copied bcm2711-rpi-4-b.dtb"
    fi

    # Firmware files
    RPI_FW="https://github.com/raspberrypi/firmware/raw/master/boot"
    for fw in start4.elf fixup4.dat; do
        if [ ! -f "$TFTP_ROOT/$fw" ]; then
            echo "[+] Downloading $fw from upstream..."
            curl -sSL "$RPI_FW/$fw" -o "$TFTP_ROOT/$fw" || true
        fi
    done
    echo "[✓] TFTP setup complete."
}

do_deploy() {
    echo "================================================================"
    echo " [2/3] Building and Deploying b1nix to TFTP"
    echo "================================================================"
    if [ ! -d "$TFTP_ROOT" ]; then
        do_setup
    fi

    # KERNEL_BASE is not optional on this board. The kernel is not position
    # independent: boot.S copies the image to the address it was linked at,
    # and the default is 0x40080000 - one gigabyte in, which is past the end
    # of a 1 GB Pi 4's RAM. Building without this produces a kernel8.img that
    # relocates itself into memory that does not exist.
    echo "[+] Building AArch64 kernel (KERNEL_BASE=0x80000 for a Pi's RAM at 0)..."
    make -C "$PROJECT_DIR" ARCH=aarch64 KERNEL_BASE=0x80000

    IMAGE_SRC="$PROJECT_DIR/build/aarch64/Image"
    if [ ! -f "$IMAGE_SRC" ]; then
        echo "[-] Error: $IMAGE_SRC not found."
        exit 1
    fi

    cp -f "$IMAGE_SRC" "$TFTP_ROOT/kernel8.img"
    if [ -f "$PROJECT_DIR/tools/dts/bcm2711-rpi-4-b.dtb" ]; then
        cp -f "$PROJECT_DIR/tools/dts/bcm2711-rpi-4-b.dtb" "$TFTP_ROOT/bcm2711-rpi-4-b.dtb"
    fi

    echo "[✓] Deployed Image -> $TFTP_ROOT/kernel8.img"
    ls -lh "$TFTP_ROOT/kernel8.img"
}

do_uart() {
    echo "================================================================"
    echo " [3/3] UART Test Monitor"
    echo "================================================================"
    PORT="${SERIAL_PORT:-}"
    if [ -z "$PORT" ]; then
        PORT="$(auto_detect_port || true)"
    fi

    if [ -z "$PORT" ]; then
        echo "[-] Error: No serial port found. Please connect USB-UART or specify --port /dev/..."
        exit 2
    fi

    mkdir -p "$PROJECT_DIR/smoke_run"
    TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
    LOG_FILE="$PROJECT_DIR/smoke_run/rpi4-boot-$TIMESTAMP.log"

    echo " Serial Port : $PORT @ $BAUD_RATE 8N1"
    echo " Timeout     : ${TIMEOUT}s"
    echo " Log File    : $LOG_FILE"
    if [ -n "$WEBHOOK_URL" ]; then
        echo " Webhook     : $WEBHOOK_URL"
    fi
    echo "================================================================"
    echo "[*] Listening for Raspberry Pi 4 serial output..."

    # Use embedded python monitor for rock-solid cross-platform serial + webhook handling
    python3 - <<PYEOF
import sys, os, glob, time, json, select, urllib.request

port = "$PORT"
baud = int("$BAUD_RATE")
timeout = int("$TIMEOUT")
log_path = "$LOG_FILE"
webhook = "$WEBHOOK_URL"

class PosixSerial:
    def __init__(self, dev, rate):
        import termios, tty
        self.fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        tty.setraw(self.fd)
        b_map = {115200: termios.B115200, 57600: termios.B57600, 38400: termios.B38400, 9600: termios.B9600}
        b_val = b_map.get(rate, termios.B115200)
        termios.cfsetispeed(attrs, b_val)
        termios.cfsetospeed(attrs, b_val)
        attrs[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP | termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
        attrs[1] &= ~termios.OPOST
        attrs[2] &= ~(termios.CSIZE | termios.PARENB)
        attrs[2] |= termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
    def read(self, t=1.0):
        r, _, _ = select.select([self.fd], [], [], t)
        if r:
            try: return os.read(self.fd, 4096)
            except OSError: return b""
        return b""
    def close(self):
        if self.fd is not None:
            os.close(self.fd); self.fd = None

try:
    import serial
    ser = serial.Serial(port, baudrate=baud, timeout=1.0)
except ImportError:
    ser = PosixSerial(port, baud)

log_f = open(log_path, "w", encoding="utf-8", errors="replace")
start_time = time.time()
last_recv = time.time()
passed = False
panicked = False
exit_seen = False
line_buf = ""

try:
    while True:
        if (time.time() - start_time) > timeout:
            print("\n[-] TIMEOUT reached.")
            break
        chunk = ser.read(0.5)
        if chunk:
            last_recv = time.time()
            text = chunk.decode("utf-8", errors="replace")
            log_f.write(text)
            log_f.flush()
            sys.stdout.write(text)
            sys.stdout.flush()
            line_buf += text
            while "\n" in line_buf:
                line, line_buf = line_buf.split("\n", 1)
                line_clean = line.strip()
                if any(p in line_clean for p in ["[PANIC]", "KERNEL PANIC", "assertion failed"]):
                    panicked = True
                if "B1NIX-TEST: done" in line_clean or "M12-SMOKE: done" in line_clean:
                    passed = True
                if "INIT-EXIT: init exited" in line_clean or "B1NIX-TEST: done" in line_clean:
                    exit_seen = True
        if (passed or panicked or exit_seen) and (time.time() - last_recv >= 2.0):
            print("\n[+] Test execution completed.")
            break
finally:
    ser.close()
    log_f.close()

duration = time.time() - start_time
success = passed and not panicked
status = "PASS" if success else "FAIL"

print("\n================================================================")
print(f" Test Status : {status}")
print(f" Duration    : {duration:.2f}s")
print(f" Log File    : {log_path}")
print("================================================================")

if webhook:
    print(f"[*] Posting results to webhook: {webhook}...")
    try:
        with open(log_path, "r", errors="replace") as f:
            log_body = f.read()
        payload = json.dumps({
            "platform": "Raspberry Pi 4",
            "arch": "aarch64",
            "status": status,
            "passed": success,
            "panicked": panicked,
            "duration_seconds": round(duration, 2),
            "timestamp": "$TIMESTAMP",
            "log_content": log_body
        }).encode("utf-8")
        req = urllib.request.Request(webhook, data=payload, headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=10) as r:
            print(f"[+] Webhook response: {r.status}")
    except Exception as e:
        print(f"[-] Webhook error: {e}")

sys.exit(0 if success else 1)
PYEOF
}

case "$COMMAND" in
    setup)
        do_setup
        ;;
    deploy)
        do_deploy
        ;;
    uart)
        do_uart
        ;;
    run)
        do_setup
        do_deploy
        do_uart
        ;;
esac
