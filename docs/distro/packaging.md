# Packaging specification

What each overlay package contains, depends on and does at install time. This
is the contract phase A implements; [roadmap.md](roadmap.md) tracks whether it
exists yet, and [plan.md](plan.md) says why the set is this small.

Source layout:

```
packaging/
  b1nix-kernel/debian/          # arch-specific, built per arch
  b1nix-kernel-headers/debian/
  b1nix-firmware/debian/
  b1nix-base-files/debian/
  b1nix-desktop/debian/
  b1nix-artwork/debian/
  b1nix-installer-config/debian/
  b1nix-tools/debian/
  b1cc/debian/
tools/packages/
  build-deb.sh                  # sbuild in a pinned trixie chroot
  publish-repo.sh               # aptly, signs, writes the static tree
```

Every package: `Maintainer` is the project address, `Section: admin` (or
`x11`/`devel` where it fits), `Priority: optional`, `Homepage` the website, and
a `debian/copyright` in machine-readable format. Versions come from `git
describe` as [../versioning.md](../versioning.md) describes — never typed.

## b1nix-kernel

The only package with real logic in it.

- **Architecture** `amd64` / `arm64`, built separately, never `all`.
- **Contents** `/boot/b1nix-<release>` (the kernel, **stripped**),
  `/boot/System.map-<release>`, `/lib/modules/<release>/` (built-in module
  metadata and any real modules), `/usr/share/doc/b1nix-kernel/`.
- **Stripped, and why it matters**: the linked kernel is 53 MB, of which 45 MB
  is DWARF; stripped it is 7.9 MB. That difference is copied to the ESP and
  read by the bootloader on every boot, so the debug info ships separately.
- **Package name carries the release**: `b1nix-kernel-<release>`, with a
  `b1nix-kernel` metapackage depending on the newest. This is what makes two
  kernels installable at once, and it is the mechanism the whole fallback story
  needs — a single package that replaces itself cannot keep a known-good
  kernel.
- **Depends** `initramfs-tools`, `limine`.
- **Provides** `linux-image` so that packages depending on "a kernel" are
  satisfied. It does **not** `Conflicts:` Debian's kernel — a user who installs
  one gets a second boot entry, which is a useful escape hatch, not a bug.
- **postinst**
  1. `update-initramfs -c -k <release>`.
  2. Copy kernel and initramfs to the ESP.
  3. Regenerate the Limine configuration from the template: the new kernel
     first, the previous one second, a rescue entry last.
  4. Seed the boot counter for the new entry.
- **prerm** refuses to remove the running kernel and refuses to remove the last
  known-good one. A package that can leave a machine unbootable through a
  normal `apt autoremove` is a defect, not a policy question.
- **postrm** removes its ESP files and rewrites the configuration.

## b1nix-kernel-dbg

- The unstripped kernel at `/usr/lib/debug/boot/b1nix-<release>`, indexed by
  build id so `gdb` and the crash-triage scripts find it without being told.
- `Architecture` and version match `b1nix-kernel-<release>` exactly; a mismatch
  is worse than absence, because the symbols will be wrong rather than missing.
- Never on the ISO, never a dependency of anything. It exists in the repo for
  whoever is debugging a panic, and it is attached to the GitHub release as
  well, so a symbolised backtrace does not require adding an apt source first.

## b1nix-kernel-headers

- Headers and the minimum build tree for out-of-tree modules, at
  `/usr/src/b1nix-headers-<release>/`.
- `Architecture` matches the kernel package; `Depends` on the matching
  `b1nix-kernel-<release>`.
- Ships no promise: `README.Debian` states that DKMS packages are not
  supported, only enabled.

## b1nix-firmware

- `Architecture: all`, firmware under `/lib/firmware/`, each blob beside its
  license.
- Only blobs Debian's `firmware-*` packages do not provide in a usable layout;
  `Breaks`/`Replaces` nothing, so it never fights Debian's firmware packages.
- Anything without a redistribution grant is **not** in the package: it is
  fetched at install time by a script that states what it downloads and from
  where, or it is simply documented.

## b1nix-base-files

The package that makes an installed Debian into b1nix.

- `/etc/os-release` and `/usr/lib/os-release` — `ID=b1nix`, `ID_LIKE=debian`,
  the release number and codename, `B1NIX_KERNEL`.
- `/etc/apt/sources.list.d/b1nix.list` and the signing key in
  `/etc/apt/keyrings/`. The repo URL lives here and nowhere else, so moving it
  later is a package update.
- `/etc/apt/preferences.d/b1nix-kernel` — the pin that keeps Debian's
  `linux-image-*` from being pulled in by a dependency chain.
- `/etc/issue`, `/etc/motd`, default `sysctl.d` and `udev` rules.
- `Essential: no`, but `Priority: required`, and the installer always installs
  it.

## b1nix-desktop

`Architecture: all`, no files worth mentioning: the dependency list is the
package. Plasma minimal rather than `kde-standard` — every extra application is
another way to find a kernel bug during a release, and the ISO has a size
budget to keep.

**Depends** — a desktop that does not boot without them:

| Package | Why |
|---|---|
| `plasma-desktop`, `plasma-workspace-wayland` | the session itself; not `kde-full`, not `kde-standard` |
| `sddm` | the display manager, on Wayland |
| `pipewire`, `pipewire-pulse`, `wireplumber` | audio, as Debian ships it |
| `network-manager`, `network-manager-gnome`-equivalent plasma applet | networking the user can see |
| `fonts-dejavu`, `fonts-noto-core`, `fonts-noto-color-emoji` | Latin, Cyrillic and emoji coverage; missing glyphs are the most visible kind of unfinished |
| `xdg-desktop-portal-kde` | file dialogs and screen sharing from sandboxed apps |
| `konsole` | a terminal is not optional on this system |
| `b1nix-base-files`, `b1nix-artwork` | the identity |

**Recommends** — expected, removable, and the system still works without them:
`dolphin`, `kate`, `firefox-esr`, `gwenview`, `ark`, `spectacle`,
`plasma-systemmonitor`, `kde-spectacle`-adjacent utilities, `cups` and
`system-config-printer`, `bluedevil` once Bluetooth works.

**Suggests** — named so people can find them, never installed by default:
`chromium` (the harder browser, kept as a test rather than a default),
`libreoffice`, `kdenlive`.

Rule for the list: a package moves from Recommends to Depends only when the
desktop is broken without it, and the release that moves it says so in the
notes. The list is re-read at every release; a package that has not been
started by anyone on the reference machines does not belong in Depends.

## b1nix-artwork

- Wallpapers, Plasma look-and-feel, the Plymouth theme, the bootloader theme,
  the logo in the sizes the system needs (including 16×16).
- `debian/copyright` states the license of every asset. Artwork with an unclear
  license does not ship.

## b1nix-installer-config

- Calamares branding and module configuration under `/etc/calamares/`.
- The `shellprocess` module that installs Limine, since Calamares' own
  bootloader module does not know it.
- The partitioning defaults: btrfs with `@`, `@home`, `@snapshots`; a FAT ESP
  mounted at `/boot`; ext4 offered as an alternative.
- Installed only on the live ISO, not on the installed system.

## b1nix-tools

- `b1nix-report` — collects CPU, PCI/USB ids, firmware versions, the boot log
  and driver bindings into one attachable file. The format is stable enough
  that the hardware list can be generated from submissions.
- The netconsole collector, the gdb-stub helper and the boot-timeline script,
  as they exist in `tools/debug/` today.
- `Architecture: all` where the scripts allow it.

## b1cc

- The existing compiler, packaged as it is. Its own upstream version, not the
  kernel's.

## Patched Debian packages

Rules, not a list — the list lives in `docs/distro/patched-packages.md` and
should stay empty:

- A patch is allowed only when the bug cannot be fixed in the kernel in
  reasonable time, and the entry names the kernel gap and the milestone that
  removes it.
- Version scheme `<debian version>+b1nix1`, so Debian's next upload supersedes
  it automatically and the patch disappears when it is no longer rebuilt.
- Every patched package is re-checked at each release: if the kernel gap is
  closed, the package is dropped rather than rebuilt.

## What the build must guarantee

- `build-deb.sh` runs `lintian` and fails on errors. A derivative that ships
  packages with broken dependencies teaches users to distrust `apt`.
- `publish-repo.sh` signs `Release`, writes `by-hash` indices, and refuses to
  publish if the version it is about to add sorts below what is already in the
  suite.
- Both scripts are runnable by hand with no CI, because the release must not
  depend on a hosted runner being available.
