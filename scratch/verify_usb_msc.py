import os
import subprocess
import time
import sys

PROJECT_DIR = "/Users/dmytrom/Documents/GitHub/b1nix"
USB_IMG = os.path.join(PROJECT_DIR, "scratch/usb-test.img")
LOG_FILE = os.path.join(PROJECT_DIR, "scratch/usb-boot-test.log")

def run_cmd(cmd, shell=True):
    res = subprocess.run(cmd, shell=shell, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"Command failed: {cmd}\nStdout:\n{res.stdout}\nStderr:\n{res.stderr}")
        sys.exit(1)
    return res.stdout

def setup_image():
    print("[TEST] Formatting ext4 image with label 'b1nixroot'...")
    run_cmd(f"dd if=/dev/zero of={USB_IMG} bs=1M count=8 conv=notrunc 2>/dev/null")
    mke2fs = "/opt/homebrew/opt/e2fsprogs/sbin/mke2fs"
    if not os.path.exists(mke2fs):
        mke2fs = "mke2fs"
    run_cmd(f"{mke2fs} -t ext4 -O ^metadata_csum,^64bit,^flex_bg,^huge_file -L b1nixroot -q {USB_IMG} 2>/dev/null")

def build_iso(cmdline):
    print(f"[TEST] Building ISO with KERNEL_CMDLINE='{cmdline}'...")
    run_cmd(f"make ARCH=x86_64 KERNEL_CMDLINE='{cmdline}' iso >/dev/null")

def run_qemu(timeout_sec=8):
    if os.path.exists(LOG_FILE):
        os.remove(LOG_FILE)
    qemu_cmd = (
        f"qemu-system-x86_64 "
        f"-cdrom {PROJECT_DIR}/build/x86_64/b1nix.iso "
        f"-serial stdio -display none -monitor none -no-reboot "
        f"-device isa-debug-exit,iobase=0xf4,iosize=0x04 "
        f"-device qemu-xhci,id=xhci "
        f"-drive file={USB_IMG},if=none,id=usbdisk,format=raw "
        f"-device usb-storage,drive=usbdisk,bus=xhci.0 "
        f"> {LOG_FILE} 2>&1"
    )
    p = subprocess.Popen(qemu_cmd, shell=True)
    time.sleep(timeout_sec)
    p.terminate()
    try:
        p.wait(timeout=2)
    except subprocess.TimeoutExpired:
        p.kill()
    
    with open(LOG_FILE, "r") as f:
        return f.read()

def main():
    os.makedirs(os.path.dirname(USB_IMG), exist_ok=True)
    setup_image()

    print("\n--- Test: USB MSC Boot via LABEL=b1nixroot ---")
    build_iso("root=LABEL=b1nixroot b1nix.test=1")
    output = run_qemu(8)
    
    print("Boot Output:")
    lines = output.splitlines()
    # Print the last 40 lines of the boot output to avoid spamming but show the final status
    for line in lines[-40:]:
        print(line)
    
    # Check if the USB storage device was initialized and mounted as the root filesystem
    usb_init_ok = "usb: storage device initialized size=" in output or "usb: storage device initialized" in output
    usb_mount_ok = "rootfs: usb0 mounted at / as ext4" in output or "rootfs: usb0p1 mounted at / as ext4" in output or "rootfs: usb0 mounted at / as ext3" in output or "rootfs: usb0p1 mounted at / as ext3" in output or "rootfs: usb0 mounted at / as ext2" in output or "rootfs: usb0p1 mounted at / as ext2" in output
    
    if usb_init_ok and usb_mount_ok:
        print("\n  PASS: USB Mass Storage device initialized and rootfs mounted successfully!")
    else:
        print("\n  FAIL: USB Mass Storage boot failed!")
        sys.exit(1)

if __name__ == "__main__":
    main()
