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
  and `b1nix-tools`. `b1nix-artwork`, `b1nix-installer-config` and `b1cc` are
  not packaged yet: the first two have no assets before phase D.
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
  16 checks, all passing.
- [x] `done` Proof: a clean trixie chroot adds the repository and installs
  `b1nix-kernel`, `b1nix-base-files` and `b1nix-tools` from it.
- [ ] `planned` A signing key and its rotation note; `b1cc` packaged; the
  kernel package built for `arm64` as well as `amd64`.

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
- [ ] `partial` **The image boots through Limine's BIOS path, not UEFI.** The
  kernel asks for a fixed load address at 1 MiB and the firmware is already
  there, so Limine refuses; the tree's own ISOs fail the same way under OVMF.
  The disk carries a BIOS boot partition and an ESP, so the layout is ready for
  the day the kernel becomes relocatable. See
  [../kernel/abi-gaps.md](../kernel/abi-gaps.md).
- [ ] `partial` `/boot` is not mounted in the running system: the ESP is in
  `/etc/fstab` by label and the mount does not happen, so the boot-counting
  state is invisible to userspace and no boot is ever marked good.
- [x] `done` systemd's mount namespacing works: `vfs_set_propagation` and
  `vfs_remount` match a mount by its node as well as by its path, which is what
  a bind into a prepared root needs. `systemd-udevd`, `systemd-logind`,
  `systemd-journald`, `dbus-broker` and `systemd-sysctl` all start now — the
  failed-unit list went from twelve to four.
- [ ] `partial` Four units still fail, each with its own cause, all listed in
  [../kernel/abi-gaps.md](../kernel/abi-gaps.md): `tmp.mount` and
  `run-lock.mount` (the mount succeeds but is reported under the wrong path),
  `e2scrub_reap` (`sched_setscheduler`) and `systemd-sysusers`.
- [x] `initial` The repository reaches the guest over 9p — mounted by tag with
  no options, which is all this kernel's 9p takes. `apt-get update` against it
  still fails on a `symlink()` and on apt's `store:` method.
- [ ] `planned` Boot counting as designed in
  [boot-counting.md](boot-counting.md): the initramfs hook decrements, a unit
  marks the boot good, the postinst seeds and regenerates `limine.conf`.
- [x] `initial` Lane `DISTRO-SMOKE` (`tests/distro-smoke.sh`), to the contract
  in [lanes.md](lanes.md). It boots the image, reads the in-guest checks, grades
  the failed units against `tests/support/known-degraded.txt`, and boots the
  broken-kernel image up to four times to see the fallback happen.
- [ ] `partial` The lane is not green, and says so: the kernel boots, userspace
  reaches the in-guest checks, and then `tmp.mount`, `run-lock.mount`,
  `systemd-logind`, `systemd-udevd` and the rest of the list above still fail,
  `apt` cannot reach the repository shared over 9p, and no boot is marked good
  because `/boot` is unmounted. Every one of those is a real gap, not a lane
  defect.

## Phase C: cgroup v2

- [x] `done` Kernel milestone M127, pulled ahead of M125 because systemd's
  whole model rests on it: delegation, `MemoryMax`, `CPUWeight`,
  `memory.events`, PSI.
- [x] `partial` Proof through the distribution: on the systemd lane a
  `MemoryMax=48M` unit's `tail /dev/zero` is SIGKILLed inside its own cgroup
  while PID 1 carries on, two `CPUWeight=` units at 100 and 1000 divide the CPU
  1:9.8 by their own `cpu.stat`, and `/proc/pressure/cpu` moves under that
  load. `systemd-oomd` has not been run.

Detail in [../kernel/roadmap.md](../kernel/roadmap.md) under M127.

## Phase D: live ISO and installer

- [ ] `planned` Netinstall ISO from the same debootstrap root: a graphical
  installer environment, the base system carried for an offline install, the
  desktop pulled over the network, the known-issues page shown before the first
  step.
- [ ] `planned` The size budget fixed as a number (target 900 MB–1.4 GB) and
  asserted by the lane.
- [ ] `planned` Calamares with our branding, the btrfs subvolume layout
  (`@`, `@home`, `@snapshots`), ext4 offered, LUKS optional, and a
  `shellprocess` module that installs Limine instead of Calamares' bootloader
  module.
- [ ] `planned` The apt snapshot hook and the documented rescue recipe
  (boot the ISO, chroot, reinstall the previous kernel).
- [ ] `planned` Lane `INSTALL-SMOKE`: unattended install onto a blank disk in
  QEMU, then boot the installed disk to a login; a second run with the network
  unplugged must still produce a bootable console system. Exercises Qt, udisks, parted,
  loop devices, GPT writes, btrfs and fsync.

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
(io_uring) and M126 (perf, eBPF) are closed. What the phases below actually
wait on is kernel milestone **M133**, which collects every gap in that table no
other milestone owns — UEFI boot above all, and the four Debian units that
still fail. M130 (Wi-Fi) and M135 (the rest of the power management) each land after it as
a `b1nix-kernel` release the overlay ships, and the self-host build lane stays as
a kernel test on release tags.
