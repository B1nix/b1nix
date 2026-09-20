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

- [ ] `planned` `packaging/` in the tree: `debian/` directories for
  `b1nix-kernel`, `b1nix-kernel-headers`, `b1nix-base-files`, `b1nix-desktop`,
  `b1nix-artwork`, `b1nix-installer-config`, `b1nix-tools`, `b1cc`.
- [ ] `planned` `tools/packages/build-deb.sh` (sbuild in a pinned trixie
  chroot) and `tools/packages/publish-repo.sh` (aptly, signed static tree);
  package versions derived from `git describe`, never typed.
- [ ] `planned` The signing key and its rotation note, the apt pinning that
  keeps Debian's `linux-image-*` out, `/etc/os-release`. `SECURITY.md` and
  `CONTRIBUTING.md` are written; the address in them is filled in when the
  domain exists.
- [ ] `planned` Package contents follow [packaging.md](packaging.md), which is
  the contract this phase implements.
- [ ] `planned` Proof: a Debian trixie container adds the repo, `apt install
  b1nix-kernel` succeeds, `dpkg -L` shows the kernel in `/boot`, and pinning
  refuses Debian's kernel.

No kernel work. This phase exists so that everything after it has somewhere to
ship to.

## Phase B: bootable installed system

- [ ] `planned` `tools/images/mk-b1nix-image.sh`: debootstrap trixie, install
  the overlay, produce a disk image that boots the b1nix kernel to a systemd
  multi-user target; initramfs from Debian's `initramfs-tools`.
- [ ] `planned` Limine integration: the config template, the `b1nix-kernel`
  postinst that writes it, the ESP layout (kernel and initramfs on FAT at
  `/boot`), two-kernel retention and the fallback entry.
- [ ] `planned` Boot counting as designed in
  [boot-counting.md](boot-counting.md): the initramfs hook decrements, a unit
  marks the boot good, the postinst seeds and regenerates `limine.conf`.
- [ ] `planned` Lane `DISTRO-SMOKE`, to the contract in [lanes.md](lanes.md):
  the image boots, `systemctl is-system-running` matches the known-degraded
  list, `apt update` works, and a deliberately broken kernel falls back after
  three tries.

## Phase C: cgroup v2

- [ ] `planned` Kernel milestone M127, pulled ahead of M125 because systemd's
  whole model rests on it: delegation, `MemoryMax`, `CPUWeight`,
  `memory.events`, PSI.
- [ ] `planned` Proof through the distribution: systemd slices enforce limits,
  `systemd-oomd` kills inside a cgroup, `/proc/pressure/*` moves under load.

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
  machines (the Intel-graphics laptop, the T480 over PXE, QEMU on both arches).
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
distribution is in [../kernel/abi-gaps.md](../kernel/abi-gaps.md). M130 (Wi-Fi), M129 (power and
suspend), M125 (io_uring) and M126 (perf, eBPF) each land as a
`b1nix-kernel` release the overlay ships; the self-host build lane stays as a
kernel test on release tags.
