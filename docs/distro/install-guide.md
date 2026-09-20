# Installing b1nix — draft

A draft of the user-facing guide, written early on purpose: it forces the UX
decisions while they are still cheap. The published version lives on the
website; this file is where its content is agreed.

Read the whole page before starting. b1nix runs an operating-system kernel
written from scratch. It is not a Linux distribution with a custom theme, and
it can lose your data. Install it on a machine whose contents you can afford to
lose, or on a spare disk.

## What you need

- A machine with **64-bit x86** and **UEFI** firmware. There is no BIOS boot.
- **A network cable.** b1nix has no Wi-Fi yet, and the desktop is downloaded
  during installation. Without a cable you can still install a working
  command-line system — see [Installing without a
  network](#installing-without-a-network).
- A USB stick of 2 GB or more.
- **Secure Boot turned off.** b1nix's kernel is not signed by a key your
  firmware trusts. Turning it off is a setting in your firmware menu, usually
  under Security or Boot.

## 1. Download and verify

Download the image, the checksum file and its signature from the release page,
then check them. This step matters more here than for a large distribution: the
files are hosted on a small project's infrastructure.

```
gpg --import b1nix-archive.asc          # the key, from the website
gpg --verify SHA256SUMS.asc SHA256SUMS  # must say "Good signature"
sha256sum -c SHA256SUMS                 # must say "OK"
```

If the signature does not verify, stop. Do not install the image.

## 2. Write the USB stick

```
sudo dd if=b1nix-1-amd64.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

`/dev/sdX` is the whole stick, not a partition, and everything on it is lost.

## 3. Boot it

Choose the stick in your firmware's boot menu. b1nix's boot menu appears; the
first entry starts the installer.

The known-issues page appears before anything else. Read it: it lists what does
not work in this release, and it is shorter than the list of what does.

## 4. Install

The installer asks for language and keyboard, timezone, your user, and where to
install.

- **Disk**: "erase disk" uses the whole disk with btrfs, which is what we
  test and what lets the system take a snapshot before every upgrade. Manual
  partitioning is available; keep an EFI partition of at least 512 MB, because
  b1nix stores kernels there.
- **Desktop**: the installer downloads it. If there is no network, this step is
  skipped and you get a command-line system.

Installation takes a while, most of it downloading.

## 5. First boot

Remove the stick and boot. You get a login screen, or a console if the desktop
was skipped.

## Installing without a network

The image contains a complete base system. With no cable, the installer
finishes and leaves you a working command-line system. Connect a cable later
and run:

```
sudo apt update
sudo apt install b1nix-desktop
sudo systemctl enable --now sddm
```

## If it does not boot

b1nix keeps the previous kernel installed and both appear in the boot menu.
After three failed attempts with a new kernel, the previous one becomes the
default automatically.

To recover by hand: boot the USB stick, choose the rescue entry, then

```
sudo mount /dev/sdXn /mnt              # your root partition
sudo mount /dev/sdXm /mnt/boot         # your EFI partition
sudo mount --rbind /dev /mnt/dev && sudo mount --rbind /proc /mnt/proc && sudo mount --rbind /sys /mnt/sys
sudo chroot /mnt
apt install --reinstall b1nix-kernel-<previous version>
```

## Reporting a problem

Run `b1nix-report`, attach the file it writes to an issue, and say what you
expected to happen. Reports without the file cannot usually be acted on.

If the machine does not get far enough to run anything, the fastest way to a
usable report is a serial console or a second machine collecting netconsole
output; both are documented on the website's troubleshooting page.
