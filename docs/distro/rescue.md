# Rescue

What to do when an installed b1nix does not boot, in the order to try it.
The layout this assumes is the installer's: a FAT partition mounted at `/boot`
(Limine, the kernels, the boot state) and a btrfs root with the subvolumes
`@`, `@home` and `@snapshots`.

## 1. The boot menu

Two kernels stay installed and both are in Limine's menu, with a rescue entry
for the newest that boots single-user. A kernel that fails three boots in a row
hands the default back to the previous one by itself
([boot-counting.md](boot-counting.md)), so the usual first step is to do
nothing and boot again.

## 2. Undo the last upgrade

Every `apt` run takes a snapper snapshot before and after (Debian's `snapper`
package carries the hook; the installer configures it for the root). If the
system boots but the last upgrade broke something:

```
sudo snapper list                      # find the "apt" pre snapshot
sudo snapper undochange <pre>..<post>  # put the files back
```

## 3. From the live medium

Boot the b1nix live medium; it logs in as root on its console. Then:

```
mount -o subvol=@ /dev/sdX2 /mnt          # the root partition
mount /dev/sdX1 /mnt/boot                 # the FAT partition
for d in dev proc sys run; do mount --rbind /$d /mnt/$d; done
chroot /mnt
```

`lsblk -f` names the partitions: the FAT one labelled `B1NIX-ESP`, the btrfs
one the root. Inside the chroot:

- **Rewrite the boot entries** (a damaged or missing `limine.conf`):
  `b1nix-update-bootloader`
- **Rebuild a kernel's initramfs**:
  `mkinitramfs -o /boot/initrd-<release> <release>`, then
  `b1nix-update-bootloader`
- **Reinstall the previous kernel** (its files on `/boot` are gone or
  damaged): `apt install --reinstall b1nix-kernel-<release>`, which needs the
  network, or `dpkg-reconfigure b1nix-kernel-<release>` when only its
  initramfs and boot entry need rebuilding. `ls /usr/lib/modules` lists the
  releases installed.
- **Restore a whole snapshot**: from outside the chroot, with the top level of
  the btrfs mounted (`mount -o subvolid=5 /dev/sdX2 /top`), move `@` aside
  (`mv /top/@ /top/@broken`) and snapshot the good one into its place
  (`btrfs subvolume snapshot /top/@snapshots/<n>/snapshot /top/@`).

Leave the chroot, `umount -R /mnt`, and reboot.
