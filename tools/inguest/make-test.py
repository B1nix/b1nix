#!/usr/bin/env python3
"""Fast in-guest check that the ported bmake actually runs inside b1nix.

Boots the interactive ISO with build/x86_64/root.ext4 as virtio-blk (mounts
/persist), waits for the shell, then runs:
  - /persist/bin/make -f /dev/null -V MAKE_VERSION  (proves the ELF loads + runs)
  - /persist/bin/make -C <maketest>  (drives a real Makefile: variables,
    automatic vars ${.ALLSRC}/${.TARGET}, prerequisite chaining, recipe spawn
    via /bin/sh)

The Makefile fixture (tools/inguest/maketest/) is staged into the source tree
at /persist/usr/src/b1nix by `make root-image` (install-kernel-source).

Watches for the expected markers, then a sentinel. Unlike the full self-host
build (~45 min in TCG) this is a few-second smoke of the make port itself.

Prereq: make ARCH=x86_64 root-image && make ARCH=x86_64 iso
Usage:  python3 tools/inguest/make-test.py [ram_mb] [timeout_s]
"""
import os, re, subprocess, sys, time

RAM = sys.argv[1] if len(sys.argv) > 1 else "256"
TIMEOUT = int(sys.argv[2]) if len(sys.argv) > 2 else 180
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOG = os.path.join(ROOT, "smoke_run", "inguest-make-test.log")
os.makedirs(os.path.dirname(LOG), exist_ok=True)
SENTINEL = "MK-HARNESS-DONE"
MAKETEST = "/persist/usr/src/b1nix/tools/inguest/maketest"
CMD = ("/persist/bin/make -f /dev/null -V MAKE_VERSION; "
       f"/persist/bin/make -C {MAKETEST}; "
       f"echo {SENTINEL}\n")

WANT = ["MK-AUTOVAR dep from in.txt", "MK-VARS hello world"]
# Narrow: boot prints benign "device not found" lines, so only hard faults here.
FAULTS = ("KERNEL PANIC", "[PANIC]", "EXCEPTION:", "Segmentation fault")

qemu = [
    "qemu-system-x86_64",
    "-machine", "accel=kvm:hvf:tcg",
    "-cdrom", f"{ROOT}/build/x86_64/b1nix.iso",
    "-m", RAM, "-boot", "d",
    "-serial", "stdio", "-display", "none", "-monitor", "none", "-no-reboot",
    "-drive", f"file={ROOT}/build/x86_64/root.ext4,if=none,id=vblk0,format=raw",
    "-device", "virtio-blk-pci,drive=vblk0",
]

f = open(LOG, "wb", buffering=0)
p = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=f, stderr=subprocess.STDOUT)
sent = False
status = "timeout"
start = time.time()
try:
    while time.time() - start < TIMEOUT:
        time.sleep(1)
        rc = p.poll()
        text = open(LOG, "rb").read().decode("latin1", "replace")
        if not sent and ("Welcome to b1nix shell" in text or "/> " in text):
            time.sleep(1.0)
            p.stdin.write(CMD.encode()); p.stdin.flush(); sent = True
            print(f"[{int(time.time()-start)}s] sent make-test command", flush=True)
        if re.search(rf"^\s*{re.escape(SENTINEL)}\s*$", text.replace("\r", "\n"), re.M):
            status = "done"; break
        # only treat faults as fatal AFTER the command was sent (boot banners are noisy)
        if sent and any(m in text for m in FAULTS):
            status = "guest-fault"; break
        if rc is not None:
            status = f"qemu-exit-{rc}"; break
finally:
    if p.poll() is None:
        p.terminate(); time.sleep(1)
        if p.poll() is None: p.kill()

text = open(LOG, "rb").read().decode("latin1", "replace")
elapsed = int(time.time() - start)
print(f"=== status={status} ram={RAM}MB elapsed={elapsed}s log={LOG} ===")
for w in WANT:
    print(f"  [{'OK' if w in text else 'MISS'}] {w}")
allok = status == "done" and all(w in text for w in WANT)
print("RESULT:", "PASS" if allok else "FAIL")
sys.exit(0 if allok else 1)
