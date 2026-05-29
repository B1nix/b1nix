#!/usr/bin/env python3
"""Drive an in-guest b1nix kernel self-host build over the serial console.

Portable across hosts: QEMU accel falls back kvm -> hvf -> tcg, so it runs fast
on a Linux/KVM bench, on macOS (HVF), or anywhere (TCG). Boots the interactive
ISO with build/x86/root.ext4 as virtio-blk (mounts /persist), optionally attaches
a swap disk as AHCI sata1, waits for the shell, runs the in-guest build, copies
the resulting kernel.elf to /persist, and watches for a completion marker.

Prereqs (build once on the bench):
  tools/build-toolchain.sh && make -C userspace && \
  tools/build-native-toolchain.sh && make ARCH=x86 root-image && make ARCH=x86 iso

Usage:
  python3 tools/inguest/run-build.py <ram_mb> [timeout_s] [swap_mb]
  e.g.  python3 tools/inguest/run-build.py 256          # 256MB, no swap
        python3 tools/inguest/run-build.py 128 2700 256 # 128MB + 256MB swap
"""
import os, re, subprocess, sys, time

RAM = sys.argv[1] if len(sys.argv) > 1 else "256"
TIMEOUT = int(sys.argv[2]) if len(sys.argv) > 2 else 2700
SWAP_MB = int(sys.argv[3]) if len(sys.argv) > 3 else 0
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "smoke_run")
os.makedirs(OUT, exist_ok=True)
LOG = os.path.join(OUT, f"inguest-{RAM}mb.log")
ARTIFACT = f"/persist/kernel-selfhost-{RAM}mb.elf"
MARKER = "HARNESS_DONE_B1NIX"
CMD = (f"sh /persist/usr/src/b1nix/tools/inguest/build-kernel.sh; "
       f"cp /tmp/kernel.elf {ARTIFACT}; sync; "
       f"ls -l /tmp/kernel.elf {ARTIFACT}; sync; echo {MARKER}\n")

def img(name, mb):
    path = os.path.join(OUT, name)
    if not os.path.exists(path) or os.path.getsize(path) != mb * 1024 * 1024:
        with open(path, "wb") as f:
            f.truncate(mb * 1024 * 1024)
    return path

qemu = [
    "qemu-system-x86_64",
    "-machine", "accel=kvm:hvf:tcg",  # fast where available, always falls back to TCG
    "-cdrom", f"{ROOT}/build/x86/b1nix.iso",
    "-m", RAM, "-boot", "d",
    "-serial", "stdio", "-display", "none", "-monitor", "none", "-no-reboot",
    "-drive", f"file={ROOT}/build/x86/root.ext4,if=none,id=vblk0,format=raw",
    "-device", "virtio-blk-pci,drive=vblk0",
]
if SWAP_MB > 0:
    # swap_init() looks for sata1/nvme1, so put a dummy on ahci.0 and swap on ahci.1
    qemu += [
        "-device", "ich9-ahci,id=ahci",
        "-drive", f"file={img('dummy-sata0.img', 8)},if=none,id=sd0,format=raw",
        "-device", "ide-hd,drive=sd0,bus=ahci.0",
        "-drive", f"file={img(f'swap{SWAP_MB}.img', SWAP_MB)},if=none,id=sd1,format=raw",
        "-device", "ide-hd,drive=sd1,bus=ahci.1",
    ]

f = open(LOG, "wb", buffering=0)
p = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=f, stderr=subprocess.STDOUT)
sent = False
status = "timeout"
start = time.time()
last = 0
FAULTS = ("KERNEL PANIC", "[PANIC]", "EXCEPTION:", "out of contiguous",
          "klarge: OOM", "pmm_reclaim_failed", "no free swap slots")
try:
    while time.time() - start < TIMEOUT:
        time.sleep(1)
        rc = p.poll()
        text = open(LOG, "rb").read().decode("latin1", "replace")
        if not sent and ("Welcome to b1nix shell" in text or "/> " in text):
            p.stdin.write(CMD.encode()); p.stdin.flush(); sent = True
            print(f"[{int(time.time()-start)}s] sent build command", flush=True)
        if re.search(rf"^\s*{re.escape(MARKER)}\s*$", text.replace("\r", "\n"), re.M):
            status = "done"; break
        if any(m in text for m in FAULTS):
            status = "guest-fault"; break
        if rc is not None:
            status = f"qemu-exit-{rc}"; break
        el = int(time.time() - start)
        if el - last >= 30:
            last = el
            steps = re.findall(r"KBUILD [0-9]+ [^\r\n]+", text)
            print(f"[{el}s] {steps[-1] if steps else 'waiting for shell'} "
                  f"(log {len(text)}B)", flush=True)
finally:
    if p.poll() is None:
        p.terminate(); time.sleep(1)
        if p.poll() is None: p.kill()
    elapsed = int(time.time() - start)
    done = sum(1 for _ in re.finditer(r"^KBUILD-DONE", open(LOG).read().replace("\r","\n"), re.M))
    print(f"=== status={status} ram={RAM}MB elapsed={elapsed}s kbuild_done={done} log={LOG} ===")
    sys.exit(0 if status == "done" else 1)
