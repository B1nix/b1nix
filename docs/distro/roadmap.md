# Distribution roadmap

Status: `[x]` completed · `initial` usable first implementation · `partial`
incomplete or limited · `planned` not implemented · `deferred` postponed ·
`wontfix` declined.

b1nix the distribution is a Debian trixie derivative that boots the b1nix
kernel: Debian's glibc, systemd and archive, our kernel, and a small apt
overlay. The reasoning, the parts that are not phases, and the list of what
gets cut when time runs out are in [plan.md](plan.md). Version numbers are in
[../versioning.md](../versioning.md). The kernel's own milestones are in
[../kernel/roadmap.md](../kernel/roadmap.md).

Phases land one at a time, each ending in something installable or a new smoke
lane. Kernel milestones that a phase depends on are named where they block.

## Releases

| Release | Codename | Contents | Status |
|---|---|---|---|
| 1 | Гнилиці (`hnylytsi`) | Phases A–F: installable amd64 ISO, arm64 phone image, kernel 1.0.0 | planned |
| 2 | — | Phase G: upgrades from 1, whatever release 1's tracker says is most broken | planned |

## Phase A: packaging skeleton

- [x] `initial` `packaging/` in the tree: two source packages. `b1nix-kernel`
  produces the release-named kernel, its `-dbg` and `-headers` companions and
  the metapackage; `b1nix-meta` produces `b1nix-base-files`, `b1nix-desktop`
  and `b1nix-tools`. `b1nix-artwork` and `b1nix-installer-config` belong
  to phase D.
- [x] `initial` `tools/deb/debian-chroot.sh` builds a trixie chroot as an
  ordinary user — the registry layer the Debian lane already uses, entered
  through a user namespace, with no sudo and no container runtime.
- [x] `initial` `tools/deb/build-deb.sh` renders the packaging templates
  and builds them in that chroot under `lintian --fail-on error`; package
  versions are derived, never typed. `tools/deb/publish-repo.sh` writes a
  static apt tree with `apt-ftparchive`, signs it when `SIGN_KEY` is set, and
  refuses a version that sorts below what is already published.
- [x] `initial` The apt pin that keeps Debian's `linux-image-*` out, and
  `/usr/lib/os-release` taken over from Debian's `base-files` by diversion
  rather than by overwriting it. `.github/SECURITY.md` and `.github/CONTRIBUTING.md` are
  written; the address in them is filled in when the domain exists.
- [x] `initial` Package contents follow [packaging.md](packaging.md), which is
  the contract this phase implements.
- [x] `initial` Lane `PKG-SMOKE` (`tests/packages-smoke.sh`): builds,
  publishes, installs into a clean chroot, and asserts that the kernel is on
  `/boot`, stripped but still carrying `.kallsyms`, that `os-release` says
  b1nix, that Debian's kernel is pinned to -1, that `b1nix-report` produces its
  documented header, and that the bootloader generator writes a state file and
  a `limine.conf` offering both kernels with an uncounted rescue entry.
  16 checks at the time; see below for the rest.
- [x] `done` Proof: a clean trixie chroot adds the repository and installs
  `b1nix-kernel`, `b1nix-base-files` and `b1nix-tools` from it.
- [x] `done` The repository is signed (`SIGN_KEY`, several keys during a
  rotation) and the procedure is [archive-key.md](archive-key.md). `b1cc` is
  packaged (its own upstream version, a wrapper that defaults to the host's
  Linux target). `b1nix-kernel` is built for `arm64` too, cross-packaged from
  the aarch64 kernel, and the repository indexes both architectures.
- [x] `done` `PKG-SMOKE` grew to 25 checks: a signed publish that apt accepts
  through `Signed-By` and refuses against another key, `b1cc` compiling and
  running a C program in the clean chroot, and the arm64 kernel package
  (AArch64, stripped, `.kallsyms` kept, visible to apt as `b1nix-kernel:arm64`).

No kernel work. This phase exists so that everything after it has somewhere to
ship to.

## Phase B: bootable installed system

- [x] `initial` `tools/image/mk-b1nix-image.sh`: a trixie root with the
  overlay installed, an ESP written with mtools, a root filesystem from
  `mke2fs -d` and a GPT around them — all as an ordinary user. The image boots
  and reaches Debian's systemd; `PROFILE=broken` adds a kernel that cannot
  bring userspace up, for the fallback test.
- [x] `initial` Limine integration: the config template, the `b1nix-kernel`
  postinst that writes it, the ESP layout, two-kernel retention and the
  fallback entry.
- [x] `done` The image boots under BIOS and under UEFI: the kernel is
  relocatable (M133), and under OVMF Limine's EFI loader places it at +8 MiB.
  `DISTRO-SMOKE` boots the same image both ways and runs the in-guest checks
  under each.
- [x] `done` `/boot` is mounted and writable: the ESP is the imported Linux
  FAT (`vfat`/`msdos` through lkpi), so the boot-counting state reaches the
  disk and survives a reboot.
- [x] `done` systemd's mount namespacing works: `vfs_set_propagation` and
  `vfs_remount` match a mount by its node as well as by its path, which is what
  a bind into a prepared root needs. `systemd-udevd`, `systemd-logind`,
  `systemd-journald`, `dbus-broker` and `systemd-sysctl` all start now — the
  failed-unit list went from twelve to four.
- [x] `done` No unit fails: `tests/support/known-degraded.txt` is empty and
  the lane holds it so. The four that failed were closed in M133; three more
  boot stoppers found while reconciling this phase were kernel bugs, fixed: a
  netlink queue that dropped udev's coldplug events past sixteen (the root
  partition's device unit never appeared), mountinfo changes announced across
  mount namespaces (systemd rate-limited its mount monitor and held back every
  mount job), and `strace -p 1` leaving PID 1 stopped (PTRACE_INTERRUPT posted
  a real SIGSTOP).
- [x] `done` The repository reaches the guest over 9p, and `apt-get update`
  and `apt-get install` work against it (M133).
- [x] `done` Boot counting as designed in
  [boot-counting.md](boot-counting.md). The kernel runs a cpio initrd's
  `/init` as Linux does, so initramfs-tools' hooks run: the hook (with the
  tools it needs copied in) spends a try, switches Limine's `default_entry`
  when the tries run out, and `b1nix-boot-good` marks a boot good.
- [x] `initial` Lane `DISTRO-SMOKE` (`tests/distro-smoke.sh`), to the contract
  in [lanes.md](lanes.md). It boots the image, reads the in-guest checks, grades
  the failed units against `tests/support/known-degraded.txt`, and boots the
  broken-kernel image up to four times to see the fallback happen.
- [x] `done` The lane checks 16 things: multi-user, no unexpected failed units,
  apt over 9p, a boot marked good, `/boot`, `/tmp` and `/run/lock` mounted,
  `strace -p 1` harmless, the UEFI boot (loader, multi-user, boot marked good),
  and the fallback to the good kernel on the fourth boot of the broken image.

## Phase C: cgroup v2

- [x] `done` Kernel milestone M127, pulled ahead of M125 because systemd's
  whole model rests on it: delegation, `MemoryMax`, `CPUWeight`,
  `memory.events`, PSI.
- [x] `done` Proof through the distribution: on the systemd lane a
  `MemoryMax=48M` unit's `tail /dev/zero` is SIGKILLed inside its own cgroup
  while PID 1 carries on, two `CPUWeight=` units at 100 and 1000 divide the CPU
  1:9.8 by their own `cpu.stat`, and `/proc/pressure/cpu` moves under that
  load.
- [x] `done` `systemd-oomd` acts: a slice with
  `ManagedOOMMemoryPressure=kill` and a 10% limit holds a unit living above its
  `memory.high`; the kernel throttles it in proportion to the overage, the
  slice's own `memory.pressure` reaches ~68%, and oomd kills the unit ("due to
  memory pressure ... for > 2s"). The kernel side is per-cgroup PSI
  (`cpu/memory/io.pressure` in every cgroup) and Linux's `memory.high`
  penalty; PSI triggers are still a gap
  ([../kernel/abi-gaps.md](../kernel/abi-gaps.md)).

Detail in [../kernel/roadmap.md](../kernel/roadmap.md) under M127.

## Phase D: live ISO and installer

- [x] `done` A console live ISO from the same debootstrap root (160 MB): it
  boots under BIOS and UEFI, runs the session from compressed RAM, and carries
  the base system it installs, so an install needs no network.
- [x] `done` The size budget, 256 MB for the console medium, asserted by
  `INSTALL-SMOKE`.
- [x] `done` `b1nix-install`, a console installer in place of Calamares:
  asks its questions or reads an answer file, lays out GPT with a BIOS boot
  partition and a FAT `/boot`, btrfs with `@`, `@home`, `@snapshots` (ext4
  offered), copies the live root and installs Limine for both firmwares.
  LUKS is not offered until the kernel has dm-crypt
  ([../kernel/abi-gaps.md](../kernel/abi-gaps.md)).
- [ ] `partial` snapper set up on the btrfs root by the installer, its
  snapshots in `@snapshots`, and the rescue recipe in [rescue.md](rescue.md).
  Debian's apt hook comes with snapper, but no lane runs an upgrade on an
  installed system yet to show the snapshots appear.
- [x] `done` Lane `INSTALL-SMOKE`: two unattended installs side by side, BIOS
  with the network and UEFI without, each booting the installed disk to a
  login; about four minutes including the package and ISO builds.
- [ ] `planned` The desktop over the network after the install: b1air
  packaged in the overlay and `b1nix-desktop` pointing at it.

## Phase E: public release 1 — Гнилиці

- [ ] `planned` CI builds packages and both images on a tag, signs them, and
  publishes to GitHub Releases and Pages; a build manifest beside each image,
  and `b1nix-kernel-dbg` attached to the release.
- [ ] `planned` The website's four pages: install guide (drafted in
  [install-guide.md](install-guide.md)), hardware support (generated from
  [b1nix-report.md](b1nix-report.md) submissions), known issues, FAQ.
- [ ] `planned` The pipeline in [ci.md](ci.md), starting with the measurement
  job that decides what can run in the cloud at all.
- [ ] `planned` [release-checklist.md](release-checklist.md) is followed
  literally; a step that needs judgement is rewritten until it does not.
- [ ] `planned` `docs/release-checklist.md` run end to end on the reference
  machines (the Intel-graphics laptop, the UHD 620 laptop over PXE, QEMU on
  both arches).
- [ ] `planned` Kernel `1.0.0` cut with the release.

## Phase F: arm64 in the same release

- [ ] `planned` The overlay built for `arm64`; the phone image installs the
  same `b1nix-kernel` package and the same Debian root, with `apt` against the
  same repo.
- [ ] `planned` Flashing and recovery documented as carefully as the desktop
  install, including the way back to stock firmware.
- [ ] `planned` Scope stated honestly: no modem, no telephony, no battery-life
  claim. Detail in [../kernel/xperia5-ufs-usb.md](../kernel/xperia5-ufs-usb.md).

## Phase G: the second release and upgrades

- [ ] `planned` Lane `UPGRADE-SMOKE`: release 1 upgrades to release 2 through
  apt, reboots, still works.
- [ ] `planned` The hardware compatibility list carries entries from someone
  other than us, fed by `b1nix-report`.
- [ ] `planned` Whatever release 1's issue tracker says is most broken.

## Cleanup

Not a phase: filler work between phases, tracked in [cleanup.md](cleanup.md).
Two items come before phase A, because they would otherwise be packaged by
accident — `archive/`, and whatever the Alpine rootfs overlay sets that the
distribution must not inherit.

## Running alongside

Kernel work does not stop for the phases. What each remaining gap costs the
distribution is in [../kernel/abi-gaps.md](../kernel/abi-gaps.md). M125
(io_uring) and M126 (perf, eBPF) are closed, and so is **M133**, which
collected every gap in that table no other milestone owned — UEFI boot above
all, and the Debian units that failed. M130 (Wi-Fi) and M135 (the rest of the power management) each land as
a `b1nix-kernel` release the overlay ships, and the self-host build lane stays as
a kernel test on release tags.
