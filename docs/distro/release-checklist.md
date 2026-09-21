# Release checklist

Mechanical, in order, no judgement calls left in it. A release that skips a
step is not released; a step that turns out to need judgement gets rewritten
until it does not.

## Before the tag

- [ ] The full smoke suite is green on `x86_64` and `aarch64`, in the
      foreground, on a clean build.
- [ ] `DISTRO-SMOKE`, `INSTALL-SMOKE`, `DESKTOP-SMOKE` and, from release 2,
      `UPGRADE-SMOKE` are green.
- [ ] The soak run has completed without a new fault.
- [ ] No test was disabled or marked unsupported to reach green.
- [ ] `pgrep qemu` is empty.
- [ ] Every open item promised for this release in
      [roadmap.md](roadmap.md) is either done or moved, with the move written
      down.
- [ ] `docs/distro/patched-packages.md` re-checked: every patch still needed,
      each with a kernel gap and a milestone.
- [ ] The kernel version is bumped and committed, and the tag exists
      ([../versioning.md](../versioning.md)).

## Hardware

Each reference machine boots the release candidate and reaches a desktop or a
login:

- [ ] QEMU, both arches.
- [ ] The Intel-graphics laptop.
- [ ] The UHD 620 laptop over PXE.
- [ ] The SM8150 phone over fastboot, with the way back to stock verified.
- [ ] Suspend and resume once power management is claimed to work.
- [ ] Wi-Fi associates once it is claimed to work.

## The installed system

- [ ] Calamares installs onto a blank disk, unattended and interactively.
- [ ] The same install with no network produces a bootable console system, and
      `apt install b1nix-desktop` afterwards completes it.
- [ ] The installed system boots, `systemctl is-system-running` matches the
      known set, sound plays, the browser starts.
- [ ] `apt update && apt upgrade` works against the overlay and Debian.
- [ ] Two kernels are installed and both boot.
- [ ] A deliberately broken kernel falls back to the previous entry after three
      tries.
- [ ] The rescue recipe in the install guide works as written, word for word.

## Artifacts

- [ ] amd64 ISO and arm64 phone image built from the tag by
      `tools/deb/build-deb.sh` and the image scripts.
- [ ] `SHA256SUMS` written and signed; the signature verifies with the
      published key on a machine that does not hold it.
- [ ] The build manifest lists every Debian package version, the kernel commit
      and the toolchain versions.
- [ ] The ISO is under the size budget, and the actual size is recorded in the
      release notes.
- [ ] `b1nix-kernel-dbg` is published and its build id matches the shipped
      kernel; a captured panic symbolises with it.
- [ ] `lintian` is clean on every overlay package.
- [ ] The repo publishes and a fresh Debian container installs
      `b1nix-kernel` from it.

## Words

- [ ] Release notes: what is new, what is fixed, what is still broken, what to
      do before upgrading.
- [ ] Known issues updated and linked from the ISO's first boot.
- [ ] The install guide's first step is still signature verification, and the
      commands in it are the current ones.
- [ ] The hardware list reflects what was actually tested this cycle.
- [ ] Performance numbers for the release recorded: boot to login, login to
      desktop, browser start.

## After publishing

- [ ] Download the published ISO on a machine that built nothing, verify the
      signature and install it.
- [ ] The apt repo works from outside the build host.
- [ ] The tag is pushed and the release page links the source tag, as the GPL
      requires.
