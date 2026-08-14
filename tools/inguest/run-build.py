#!/usr/bin/env python3
"""Drive an in-guest b1nix kernel self-host build over the serial console.

Portable across hosts: QEMU accel falls back kvm -> hvf -> tcg, so it runs fast
on a Linux/KVM bench, on macOS (HVF), or anywhere (TCG). Boots the interactive
ISO with build/x86_64/root.ext4 as virtio-blk (mounts /persist), optionally attaches
a swap disk as AHCI sdb, waits for the shell, runs the in-guest build, copies
the resulting kernel.elf to /persist, and watches for a completion marker.

Prereqs (build once on the bench):
  tools/toolchain/build-toolchain.sh && make -C userspace && \
  tools/toolchain/build-native-toolchain.sh && make ARCH=x86_64 root-image && make ARCH=x86_64 iso

Usage:
  python3 tools/inguest/run-build.py <ram_mb> [timeout_s] [swap_mb]
  e.g.  python3 tools/inguest/run-build.py 256          # 256MB, no swap
        python3 tools/inguest/run-build.py 128 2700 256 # 128MB + 256MB swap
"""
import os, re, subprocess, sys, time

RAM = sys.argv[1] if len(sys.argv) > 1 else "256"
TIMEOUT = int(sys.argv[2]) if len(sys.argv) > 2 else 2700
SWAP_MB = int(sys.argv[3]) if len(sys.argv) > 3 else 0
SMP = sys.argv[4] if len(sys.argv) > 4 else "1"      # guest vCPUs (-smp N)
JOBS = sys.argv[5] if len(sys.argv) > 5 else "1"     # parallel make (-jN)
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "smoke_run")
os.makedirs(OUT, exist_ok=True)
LOG = os.path.join(OUT, f"inguest-{RAM}mb-smp{SMP}-j{JOBS}.log")
ARTIFACT = f"/persist/kernel-selfhost-{RAM}mb.elf"
MARKER = "HARNESS_DONE_B1NIX"
# Drive the in-guest build through the real parallel Makefile so -jN is
# meaningful (the flat build-kernel.sh is serial). `time` brackets the build so
# the in-guest wall-clock is captured independently of host-side polling jitter.
MK = "/persist/usr/src/b1nix/tools/inguest/Makefile"
CMD = (f"echo BUILD_BEGIN; date; "
       f"/persist/bin/make -f {MK} CC=/persist/bin/gcc LD=/persist/bin/ld -j{JOBS}; "
       f"date; echo BUILD_END; "
       f"cp /tmp/kernel.elf {ARTIFACT}; sync; "
       f"ls -l /tmp/kernel.elf {ARTIFACT}; sync; echo {MARKER}\n")

def img(name, mb):
    path = os.path.join(OUT, name)
    if not os.path.exists(path) or os.path.getsize(path) != mb * 1024 * 1024:
        with open(path, "wb") as f:
            f.truncate(mb * 1024 * 1024)
    return path

# Accelerator. Default to round-robin TCG: counter-intuitively it is FASTER than
# MTTCG (tcg,thread=multi) for this workload — emulating x86's strong (TSO) memory
# model on an ARM host forces expensive barriers per atomic, and b1nix's BKL-heavy
# kernel makes 4 real threads spin-contend rather than compute. Override for a
# correctness stress test (ACCEL=tcg,thread=multi) or on a same-arch host
# (ACCEL=kvm / ACCEL=hvf). Measured here: round-robin ~5 TU/150s vs MTTCG ~2.
ACCEL = os.environ.get("ACCEL", "tcg")
qemu = [
    "qemu-system-x86_64",
    "-accel", ACCEL,
    "-cdrom", f"{ROOT}/build/x86_64/b1nix.iso",
    "-m", RAM, "-smp", SMP, "-boot", "d",
    "-serial", "stdio", "-display", "none", "-monitor", "none", "-no-reboot",
    "-drive", f"file={ROOT}/build/x86_64/root.ext4,if=none,id=vblk0,format=raw",
    "-device", "virtio-blk-pci,drive=vblk0",
]
if SWAP_MB > 0:
    # swap_init() looks for sdb/nvme1n1, so put a dummy on ahci.0 and swap on ahci.1
    qemu += [
        "-device", "ich9-ahci,id=ahci",
        "-drive", f"file={img('dummy-sda.img', 8)},if=none,id=sd0,format=raw",
        "-device", "ide-hd,drive=sd0,bus=ahci.0",
        "-drive", f"file={img(f'swap{SWAP_MB}.img', SWAP_MB)},if=none,id=sd1,format=raw",
        "-device", "ide-hd,drive=sd1,bus=ahci.1",
    ]

f = open(LOG, "wb", buffering=0)
p = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=f, stderr=subprocess.STDOUT)
sent = False
# M39: in normal boots a getty owns the serial line (/dev/ttyS0), so the
# build session must log in as root first. The legacy raw-console triggers
# below still work for boots without the inittab supervisor (single, init=).
login_sent = False
pw_sent = False
last_probe = 0.0
READY = "RB-SHELL-1234"  # probe output; the typed probe line never matches
status = "timeout"
start = time.time()
last = 0
t_begin = None   # host time when in-guest `make` started (BUILD_BEGIN seen)
t_end = None     # host time when in-guest `make` finished (BUILD_END seen)
FAULTS = ("KERNEL PANIC", "[PANIC]", "EXCEPTION:", "out of contiguous",
          "klarge: OOM", "pmm_reclaim_failed", "no free swap slots")
try:
    while time.time() - start < TIMEOUT:
        time.sleep(1)
        rc = p.poll()
        text = open(LOG, "rb").read().decode("latin1", "replace")
        if not sent:
            nl0 = text.replace("\r", "\n")
            if not login_sent and "login:" in text:
                p.stdin.write(b"root\n"); p.stdin.flush(); login_sent = True
                print(f"[{int(time.time()-start)}s] getty login prompt: sent user", flush=True)
            elif login_sent and not pw_sent and "Password:" in text:
                p.stdin.write(b"root\n"); p.stdin.flush(); pw_sent = True
                print(f"[{int(time.time()-start)}s] sent password", flush=True)
            elif pw_sent and re.search(rf"^\s*{READY}\s*$", nl0, re.M):
                p.stdin.write(CMD.encode()); p.stdin.flush(); sent = True
                print(f"[{int(time.time()-start)}s] sent build command (serial login session)", flush=True)
            elif pw_sent and time.time() - last_probe > 5:
                # Arithmetic keeps the echoed probe line distinct from its output.
                p.stdin.write(b"echo RB-SHELL-$((1000+234))\n")
                p.stdin.flush(); last_probe = time.time()
            elif not login_sent and ("Welcome to b1nix shell" in text or "/> " in text):
                p.stdin.write(CMD.encode()); p.stdin.flush(); sent = True
                print(f"[{int(time.time()-start)}s] sent build command", flush=True)
        # Host-wall-clock bracket of the build itself (the in-guest clock is
        # unreliable on TCG). BUILD_BEGIN/BUILD_END are echoed around `make`.
        # Match on its own line (the typed command also echoes "echo BUILD_END").
        nl = text.replace("\r", "\n")
        if t_begin is None and re.search(r"^\s*BUILD_BEGIN\s*$", nl, re.M):
            t_begin = time.time()
            print(f"[{int(t_begin-start)}s] BUILD_BEGIN", flush=True)
        if t_begin is not None and t_end is None and re.search(r"^\s*BUILD_END\s*$", nl, re.M):
            t_end = time.time()
            print(f"[{int(t_end-start)}s] BUILD_END  build_wallclock={int(t_end-t_begin)}s", flush=True)
        if re.search(rf"^\s*{re.escape(MARKER)}\s*$", text.replace("\r", "\n"), re.M):
            status = "done"; break
        if any(m in text for m in FAULTS):
            status = "guest-fault"; break
        if rc is not None:
            status = f"qemu-exit-{rc}"; break
        el = int(time.time() - start)
        if el - last >= 30:
            last = el
            steps = re.findall(r"KBUILD (?:cc|[0-9]+|LINK)[^\r\n]+", text)
            print(f"[{el}s] {steps[-1] if steps else 'waiting for shell'} "
                  f"(log {len(text)}B)", flush=True)
finally:
    if p.poll() is None:
        p.terminate(); time.sleep(1)
        if p.poll() is None: p.kill()
    elapsed = int(time.time() - start)
    # Read binary + latin1: the serial log contains framebuffer/console bytes
    # that are not valid utf-8 (a plain open().read() would raise mid-run).
    _logtxt = open(LOG, "rb").read().decode("latin1", "replace").replace("\r", "\n")
    done = sum(1 for _ in re.finditer(r"^KBUILD-DONE", _logtxt, re.M))
    bwc = int(t_end - t_begin) if (t_begin and t_end) else -1
    print(f"=== status={status} ram={RAM}MB smp={SMP} jobs={JOBS} "
          f"build_wallclock={bwc}s total_elapsed={elapsed}s kbuild_done={done} log={LOG} ===")
    sys.exit(0 if status == "done" else 1)
