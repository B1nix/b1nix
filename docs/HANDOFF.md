# b1nix — Cross-Host Handoff

Purpose: pick up b1nix work on a **different machine** (e.g. the native-x86 /
Fedora+KVM rig, which is much faster than the macOS/TCG host) with full context.
Next focus: **real NIC drivers for internet on bare metal**, alongside **M37
(real hardware booting)**.

## 1. Restore the assistant's project memory on the new host

The accumulated project memory is internal working notes, so it is **not**
committed to the repo (it's gitignored under `docs/agent-memory/`). It travels as
a **self-contained tarball** instead — that's the "file" to carry over.

```sh
# on the OLD host: copy the bundle to the new machine, e.g.
scp b1nix-claude-memory.tar.gz  newhost:~/

# on the NEW host (after cloning b1nix there):
tar xzf b1nix-claude-memory.tar.gz
cd b1nix-claude-memory
sh install.sh /absolute/path/to/your/b1nix/checkout
```

`install.sh` copies the memory files into this host's
`~/.claude/projects/<encoded-repo-path>/memory/` — Claude Code keys memory by the
project's absolute path with `/`→`-`, derived automatically from the path you
pass. Then start a Claude Code session from the checkout; `MEMORY.md` loads as
the index.

(Regenerate the tarball any time from a host that has the live memory with
`tools/install-claude-memory.sh`'s sibling logic, or just re-tar
`docs/agent-memory/`. If you ever do drop the memory files into
`docs/agent-memory/` on a host, `sh tools/install-claude-memory.sh` installs them
with no path argument.)

## 2. Where the project stands (2026-06-03)

- **Milestones M0–M36 done**, plus M32a/M32b/**M32c just completed**
  (external SSH — `ssh -p 2222 root@127.0.0.1` from host into the guest works;
  see `docs/m32c-external-ssh.md`).
- **Architectures:** `ARCH=x86_64` (primary) and `ARCH=x86` (genuine i386/ELF32).
  Both build; full smoke is **369/0** on x86, single-CPU green on x86_64. We have
  **switched back to x86_64 as the working arch.** `tests/smoke.sh` defaults to
  `x86_64`.
- **Networking is now on by default** (DHCP runs whenever a NIC is present unless
  `b1nix.net=off`).
- **Net stack today is virtio-net only** (QEMU). ARP, IPv4/IPv6, ICMP/ICMPv6,
  UDP, TCP (Reno + SACK-less fast-retransmit), DHCP, DNS, NTP, SLAAC/NDP all
  implemented and smoke-tested over the virtio path + a `127.0.0.1` loopback fast
  path. Ported clients: curl (mbedTLS), wget (OpenSSL), Dropbear SSH.

## 3. Open issues to be aware of

- **SMP smoke flake (pre-existing, NOT a regression):** under `-smp 4` the suite
  intermittently stalls in the loopback-SSH / net_task section (recoverable
  stalls accumulate and eat the time budget). Single-CPU is always green. Tracked
  in `project_m32b_smp_races.md`. Root area: SIGCHLD-on-exit + poll/select EINTR
  for custom handlers; the fatal pty-drain variant was already fixed (758361c).
- **macOS host is TCG-only** (no KVM) → x86_64 emulates slowly; smoke timeouts
  are arch-aware now (240s for x86_64). On the Fedora/KVM rig use
  `-machine accel=kvm` for a large speedup (and the ≤2048MB in-guest self-host
  build, which can't run under TCG, becomes feasible — see
  `project_m26_selfhost.md`).

## 4. Next focus — real NIC drivers + M37

### Why
QEMU gives us virtio-net, but **real hardware has Intel/Realtek NICs**, not
virtio. To get b1nix on the internet on bare metal we need at least one real
PCI Ethernet driver feeding the existing L2/L3/L4 stack.

### The plumbing is already there
The networking core is driver-agnostic above L2. A new NIC driver only has to:
1. Probe its PCI device (vendor/device ID) — `kernel/dev/pci.c` enumerates the
   bus; `net_scan_pci_adapters()` / `virtio_net_probe()` in `kernel/net/net.c`
   show the registration pattern to copy.
2. Set up DMA RX/TX rings + the IRQ handler, and on RX hand each frame to
   `ethernet_receive(buf, len)` (`kernel/net/ethernet.c`) — exactly what
   `net_poll()`'s virtio RX loop does today.
3. Provide a TX function wired into the same path `net_send_ethernet()` uses.
4. Honour the kernel rules: MMIO via `vmm_map_mmio()` (not raw phys), DMA
   buffers must not straddle pages onto non-contiguous frames (see
   `project_virtio_blk_dma_straddle.md` — same class of bug bit the block layer),
   and any shared state is SMP-locked with `spin_lock_irqsave`.

### Suggested first targets (well-documented, common on real boxes & in QEMU)
- **Intel e1000 / e1000e (82540EM / 82574L)** — simple descriptor rings, great
  docs, `-device e1000`/`-device e1000e` in QEMU for A/B testing against virtio.
- **Realtek RTL8139** (trivial) then **RTL8169/8168** (gigabit, in many laptops).
- Register the driver in `net_init()` and add an `M37-NIC: ok …` smoke marker;
  use `-device e1000,netdev=net0` in a dedicated test (mirror
  `tests/ssh-hostfwd.sh`) so loopback/virtio smokes stay deterministic.

### M37 (real hardware booting) — current state
`project_baremetal_bringup.md`: **first real-HW boot already confirmed** (8-core
box + Acer ZG5). ACPI dynamic discovery, IOAPIC routing, LAPIC-timer scheduling,
PS/2 keyboard-from-timer-tick, RAM sizing all done. Roadmap M37 remaining:
UEFI/GRUB-USB bootloader, dynamic VBE/GOP mode-setting. Real NIC drivers are the
natural companion: a machine you can SSH into (M32c) over its real NIC (this
work) is "administrable over the network" on bare metal.

## 5. Build / test cheat-sheet

```sh
make ARCH=x86_64 iso                         # build kernel + ISO (primary arch)
sh tests/smoke.sh x86_64                      # full smoke (auto 240s timeout)
sh tests/smoke.sh x86                         # 32-bit suite (369/0)
sh tests/ssh-hostfwd.sh x86_64               # external SSH host->guest
make ARCH=x86_64 KERNEL_CMDLINE="b1nix.test=1" iso   # test-mode ISO by hand
```

On KVM, add `EXTRA_QEMU_ARGS="-machine accel=kvm"` (or edit the runner) for a big
speed-up. Read `CLAUDE.md` for the full build/toolchain/test rules and the
"no fake passes / fix the real bug" discipline. The roadmap (`docs/roadmap.md`)
is the source of truth for milestone status.

## 6. Key references (in `docs/agent-memory/` once installed)

- `project_baremetal_bringup.md` — real-HW boot fixes & gotchas.
- `project_x86_32bit_port.md` — the i386 port (arch-parity reference).
- `project_virtio_blk_dma_straddle.md` — DMA-straddle bug pattern (applies to
  any new DMA driver, including NICs).
- `project_m32c_external_ssh.md` / `docs/m32c-external-ssh.md` — external SSH.
- `project_m32b_smp_races.md` — the open SMP flake.
- `reference_dev_env.md` — host/runner notes.
