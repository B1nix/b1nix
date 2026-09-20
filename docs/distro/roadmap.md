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
  keeps Debian's `linux-image-*` out, `/etc/os-release`, `SECURITY.md`, the
  DCO note.
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
- [ ] `planned` Boot counting: the ESP counter seeded by the postinst, cleared
  by a unit on a good boot, reflected in Limine's entry order.
- [ ] `planned` Lane `DISTRO-SMOKE`: the image boots, `systemctl
  is-system-running` is `running` or a known `degraded` set, `apt update`
  works, and a deliberately broken kernel falls back after three tries.

## Phase C: cgroup v2

- [ ] `planned` Kernel milestone M127, pulled ahead of M125 because systemd's
  whole model rests on it: delegation, `MemoryMax`, `CPUWeight`,
  `memory.events`, PSI.
- [ ] `planned` Proof through the distribution: systemd slices enforce limits,
  `systemd-oomd` kills inside a cgroup, `/proc/pressure/*` moves under load.

Detail in [../kernel/roadmap.md](../kernel/roadmap.md) under M127.

## Phase D: live ISO and installer

- [ ] `planned` Live ISO from the same debootstrap root: squashfs, overlayfs,
  autologin into Plasma, the known-issues page on first boot.
- [ ] `planned` Calamares with our branding, the btrfs subvolume layout
  (`@`, `@home`, `@snapshots`), ext4 offered, LUKS optional, and a
  `shellprocess` module that installs Limine instead of Calamares' bootloader
  module.
- [ ] `planned` The apt snapshot hook and the documented rescue recipe
  (boot the ISO, chroot, reinstall the previous kernel).
- [ ] `planned` Lane `INSTALL-SMOKE`: unattended install onto a blank disk in
  QEMU, then boot the installed disk to a login. Exercises Qt, udisks, parted,
  loop devices, GPT writes, btrfs and fsync.

## Phase E: public release 1 — Гнилиці

- [ ] `planned` CI builds packages and both images on a tag, signs them, and
  publishes to GitHub Releases and Pages; a build manifest beside each image.
- [ ] `planned` The website's four pages: install guide, hardware support,
  known issues, FAQ.
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

## Running alongside

Kernel work does not stop for the phases. M130 (Wi-Fi), M129 (power and
suspend), M125 (io_uring) and M126 (perf, eBPF) each land as a
`b1nix-kernel` release the overlay ships; the self-host build lane stays as a
kernel test on release tags.
