# b1nix as a distribution — the full plan

Decision of 2026-09-20: the kernel-only direction of M121 is reversed. b1nix
becomes a Debian derivative with its own kernel, its own small apt overlay and
a public release, on amd64 and arm64.

This document is deliberately maximalist: it writes down the whole system as it
would exist if everything were finished, so that the parts that get cut are cut
knowingly. The phases, their status and the releases live in
[roadmap.md](roadmap.md); [What gets cut first](#what-gets-cut-first) says what
to drop when time runs out. The kernel's own milestones are in
[../kernel/roadmap.md](../kernel/roadmap.md).

## Contents

- [Shape of the product](#shape-of-the-product)
- [The overlay repository](#the-overlay-repository)
- [Why Debian and systemd help the kernel](#why-debian-and-systemd-help-the-kernel)
- [Identity](#identity)
- [Licensing and legal](#licensing-and-legal)
- [Security](#security)
- [Bootloader and disk layout](#bootloader-and-disk-layout)
- [Update, rollback and recovery](#update-rollback-and-recovery)
- [The image and its size](#the-image-and-its-size)
- [The desktop](#the-desktop)
- [Hardware support](#hardware-support)
- [Bug reporting](#bug-reporting)
- [Quality gates](#quality-gates)
- [Hosting and the repository URL](#hosting-and-the-repository-url)
- [Release engineering](#release-engineering)
- [Documentation and website](#documentation-and-website)
- [Community](#community)
- [The phone](#the-phone)
- [Phases](#phases)
- [Risks](#risks)
- [What gets cut first](#what-gets-cut-first)
- [Open decisions](#open-decisions)

## Shape of the product

| Question | Decision |
|---|---|
| Product | Own distribution `b1nix`, Debian-derivative |
| Base | Debian trixie: glibc, systemd, apt |
| Packages | Debian archive as-is + own overlay repo (10–20 packages) |
| Build | Release artifacts on the Linux host; a self-host lane on b1nix as a kernel test |
| Installer | Calamares, netinstall: the ISO carries the base system, the desktop comes over the network |
| Hosting | GitHub Releases (ISO) + GitHub Pages (apt repo), no domain at first |
| Bootloader | Limine on both arches; kernel and initramfs on the ESP |
| Root filesystem | btrfs with subvolumes, ext4 offered in the installer |
| Audience | Public release for enthusiasts |
| Arches | One distribution, amd64 + arm64 (Xperia 5 is a target of the same release) |
| Budget | Evenings and weekends — one line of work at a time |
| Versions | Four independent numbers — see [../versioning.md](../versioning.md) |

## The overlay repository

The overlay is the whole packaging burden. Everything else comes from
`deb.debian.org`, unmodified:

```
# /etc/apt/sources.list.d/b1nix.list
deb http://deb.debian.org/debian trixie main contrib non-free-firmware
deb https://<pages-host>/b1nix trixie main
```

Own packages, and nothing more:

| Package | Contents |
|---|---|
| `b1nix-kernel` | `/boot/b1nix-<release>`, arch-specific; postinst runs the bootloader and initramfs hooks |
| `b1nix-kernel-headers` | headers and the module build tree, so DKMS packages can build against our kernel |
| `b1nix-firmware` | firmware blobs the imported drivers need that Debian does not ship in a usable layout |
| `b1nix-base-files` | `/etc/os-release`, `/etc/issue`, apt sources and pinning, default sysctl/udev, the metapackage dependency list |
| `b1nix-desktop` | metapackage: Plasma, the default applications, fonts, the first-boot wizard |
| `b1nix-artwork` | Plasma look-and-feel, bootloader theme, wallpapers, Plymouth theme |
| `b1nix-installer-config` | Calamares branding and module configuration |
| `b1nix-tools` | `b1nix-report`, the netconsole collector, the gdb-stub helper, the boot-timeline script |
| `b1cc` | already ours |

Package by package, with contents, dependencies and maintainer scripts:
[packaging.md](packaging.md). What the kernel-only era left behind, and how to
prove a leftover is dead before deleting it: [cleanup.md](cleanup.md).

Plus patched Debian packages, only when a bug cannot be fixed in the kernel.
Each one carries a `debian/changelog` entry naming the kernel gap it works
around and the milestone that removes it, and a line in
`docs/patched-packages.md`. A patched package with no removal condition is a
bug in the process: the whole premise of this plan is that gaps get fixed in
the kernel.

### Pinning

Debian's own `linux-image-*` must never be installed by accident — a
dependency chain or a `dist-upgrade` will try. `b1nix-base-files` ships an apt
preference that pins `linux-image-*`, `linux-headers-*` and `initramfs-tools`'
kernel hooks to `-1`, and `b1nix-kernel` declares `Provides: linux-image`,
`Conflicts:`/`Replaces:` nothing (so a user who really wants Linux can force
it, and boot both from the menu).

### Suite naming

The overlay suite is named after the Debian suite it is built against
(`trixie`), not after the b1nix release, so that a b1nix release bump does not
invalidate anyone's `sources.list`. When b1nix rebases onto the next Debian
stable, that is a new suite and a documented upgrade path.

## Why Debian and systemd help the kernel

Every distribution feature is a kernel test. Running a stock Debian desktop
forces the milestones that are already on the roadmap, in this order:

1. **M127 cgroup v2** — systemd's whole model is cgroup v2: delegation,
   `MemoryMax`, `CPUWeight`, `memory.events`, PSI. Without it systemd runs, but
   `systemctl status`, slices, `systemd-oomd` and user sessions are hollow.
   This is the blocking milestone for calling b1nix a systemd distribution.
2. **M125 io_uring** — modern userspace assumes it; a distribution QEMU and
   `fio` prove it.
3. **M126 observability** — `perf` and eBPF: Debian ships tools that probe
   them, and they replace the ad-hoc profiling hooks in the tree.
4. **M130 hardware** — Wi-Fi (iwlwifi/mac80211) is the difference between a
   demo and a laptop someone installs.
5. **M129 power** — suspend and cpufreq; a laptop that does not suspend is not
   an installable system.

So the "kernel features" and "distribution" tracks are not in competition:
cgroup v2 first, because Calamares, systemd and a public release all rest on
it.

## Identity

- **Name** `b1nix`, always lowercase, including at the start of a sentence.
- **Codenames** one per distribution release, fixed at release, never reused.
  The theme is amusing Ukrainian village and town names. The codename goes into
  `VERSION_CODENAME` and into the apt suite in places, so it is transliterated,
  lowercase, one word, no hyphens — the Ukrainian spelling belongs in the
  release notes, the ASCII one in the files. A shortlist to draw from:
  Release 1 is **Гнилиці**, `hnylytsi` in the files. Later releases draw from
  the same theme: `vesele` (Веселе), `shchastia` (Щастя), `babyne` (Бабине),
  `krynychky` (Кринички), `rozkishne` (Розкішне).
- **One-line description**: "A Unix-like kernel written from scratch, running a
  Debian desktop." It must say both halves — the kernel is the point, the
  Debian userspace is what makes it usable — or people file the wrong bugs.
- **Tone**: honest about what does not work. The known-issues list is part of
  the release, not an appendix. An enthusiast distribution earns trust by
  saying "suspend does not work yet" before the user finds out.
- **Assets**: a logo that survives 16×16 (bootloader, favicon, Plasma menu), a
  wallpaper per release, a Plymouth theme. All in `b1nix-artwork`, all under a
  license stated in the package.
- **Domain** a single domain for the website, the apt repo and the mail
  address, even while everything is hosted on GitHub Pages behind it. Moving
  hosting later must not change anyone's `sources.list`.

## Licensing and legal

The tree is GPL-2.0 and imports Linux source (filesystems, DRM, the linuxkpi
consumers), so this is not optional bookkeeping:

- **Source availability.** Shipping binary kernels obliges us to offer the
  complete corresponding source. A public git tag per release satisfies it;
  the ISO carries a `LICENSES/` directory and the release notes link the tag.
- **Imported code provenance.** Each imported tree records upstream version
  and commit. A file that says it is Linux's must be byte-identical to that
  version, or it is a shim bug — this is already the rule for drivers, and it
  is also what keeps the license story simple.
- **Firmware.** `b1nix-firmware` redistributes blobs whose licenses mostly
  permit redistribution but not modification, and a few that permit neither.
  Every blob carries its license file; anything without a redistribution grant
  is downloaded at install time by the user, not shipped.
- **Debian trademark.** A derivative may say it is based on Debian; it may not
  use Debian's marks in a way that implies endorsement. `ID_LIKE=debian` and
  "based on Debian trixie" in the release notes are fine; Debian's logo on the
  ISO is not.
- **Contributions.** A `DCO` sign-off (`Signed-off-by:`) on contributions, no
  CLA. It matches the kernel's own practice and needs no legal entity.
- **No warranty, said plainly** in the install guide as well as the license: an
  enthusiast installing a from-scratch kernel on their laptop should be told
  that data loss is a real outcome, once, in the place they will read.

## Security

This is the section that separates "a public release" from "a tarball on a
page". None of it is hard; all of it is easy to forget.

- **Repo signing.** An OpenPGP key that exists only offline, and a subkey on
  the build host for signing `Release`. The public key ships in
  `b1nix-base-files` and is published on the website with its fingerprint. Key
  rotation is documented before it is needed, not after.
- **Artifact signing.** Every ISO has a `SHA256SUMS` and a detached signature.
  The install guide shows the verification command as the first step, not the
  last.
- **Reproducible builds**, as far as they come for free: `SOURCE_DATE_EPOCH`,
  a pinned build chroot, recorded package versions in a build manifest beside
  each ISO. Full reproducibility is not promised; the manifest is, so a build
  can be recreated.
- **Secure Boot.** Honest position: unsigned. A shim signed by Microsoft is out
  of reach for a project this size, so the install guide documents enrolling
  our key through MOK, and the default is "disable Secure Boot". Saying this in
  the docs is the deliverable; pretending otherwise would waste weeks.
- **Kernel hardening already in the tree** stays on by default — heap canaries,
  the checks that trip `kheap_validate()`. A hardening flag disabled for
  performance gets a line in the release notes.
- **Security contact**: [`SECURITY.md`](../../.github/SECURITY.md) with an address and
  a 90-day disclosure norm. No CVE process while the audience is enthusiasts, but a
  security fix is a point release, not a "next time" item.
- **The distribution's own attack surface** is mostly Debian's, and Debian's
  security updates flow through `apt` untouched. What we own is the kernel:
  memory-safety bugs in a from-scratch kernel are the realistic risk, and the
  answer is the smoke suite plus the fuzzing lane under [Quality
  gates](#quality-gates), not a policy document.

## Bootloader and disk layout

**Limine on both arches.** One bootloader for the phone, PXE and the desktop
is worth more than the integration Debian's tooling would have given GRUB —
but that integration has to be written, and it is real work:

- **Nothing in Debian knows about Limine.** `b1nix-kernel`'s postinst writes
  the Limine configuration itself, from a template in `b1nix-base-files`, and
  Calamares gets a small `shellprocess` module instead of its `bootloader` one.
  Neither is difficult; both are ours to maintain forever.
- **Boot counting is ours too.** systemd-boot would have given it for free;
  with Limine, the counter lives in a file on the ESP that the kernel's
  postinst seeds, a `systemd` unit clears on a successful boot, and the Limine
  config's entry order reflects. This must exist before the first public ISO —
  it is the mechanism the whole [rollback](#update-rollback-and-recovery)
  story rests on.
- **Kernel and initramfs live on the ESP**, formatted FAT, mounted at `/boot`.
  This keeps the bootloader off the root filesystem entirely, so Limine never
  has to read btrfs, and a root snapshot rollback and a kernel rollback stay
  independent of each other.
- **Root is btrfs** with subvolumes (`@` for `/`, `@home`, `@snapshots`) so
  that a snapshot of the system does not roll back the user's files. The apt
  hook snapshots `@` before an upgrade; recovery is booting the previous kernel
  and restoring the subvolume from a shell.
- **The btrfs risk is acknowledged**: the tree's btrfs path has produced csum
  and writeback bugs recently. The mitigation is that `INSTALL-SMOKE` and
  `UPGRADE-SMOKE` run on btrfs from the start, so the snapshot and rollback
  paths get hammered every release rather than being discovered by a user.
  ext4 stays a supported choice in the installer for anyone who wants it.
- **Swap**: zram by default, sized from RAM, with a swap file on btrfs only if
  the user asks. Real swap depends on M127's compressed swap work.

## Update, rollback and recovery

A from-scratch kernel will sometimes not boot. The distribution has to assume
that, or the first bad update ends someone's interest permanently.

- **Two kernels always.** `b1nix-kernel` keeps the previous version installed
  and both entries in the bootloader; the previous one is the fallback, and
  the postinst refuses to remove the only known-good kernel.
- **Boot counting.** Three failed tries fall back to the previous kernel
  automatically. Limine cannot write to disk, so unlike systemd-boot's version
  this is ours to build: the design, and the reasons each piece sits where it
  does, are in [boot-counting.md](boot-counting.md).
- **Filesystem snapshots.** The root is btrfs by default, so the apt hook
  snapshots the `@` subvolume before every upgrade and keeps the last few. The
  bootloader does not read them; recovery is documented as "boot the previous
  kernel, restore the subvolume".
- **A rescue path.** The ISO doubles as a rescue system: boot it, `chroot` into
  the installed root with a documented three-line recipe, reinstall the
  previous kernel. Documented in the install guide, tested in the release
  checklist.
- **Debug symbols are available but not installed.** The shipped kernel is
  stripped (7.9 MB against 53 MB linked); `b1nix-kernel-dbg` carries the DWARF
  in the repo and on the release page, matched by build id, so a user's panic
  can be symbolised without a private build.
- **`b1nix-report` on a failed boot.** The netconsole and serial paths already
  exist for development; the shipped system should be able to write a panic
  log somewhere that survives a reboot (pstore, or a file on the ESP) so a
  user can attach it to an issue.

## The image and its size

GitHub Releases refuses a file over 2 GB, and a live ISO with Plasma is 2.5–4 GB
in every distribution that ships one. Rather than host elsewhere before there is
a domain, the ISO is a **netinstall**: it installs a base system from its own
contents and pulls the desktop from the network.

The consequences are real and are not hidden from the user:

- **Installing the desktop needs a working network.** There is no Wi-Fi until
  M130, so at release 1 that means a cable. The install guide says this on the
  first screen, not in a footnote, and the installer checks for a link before
  offering the desktop.
- **The kernel's network stack becomes part of the installer.** DNS, TCP, TLS
  through `apt`, and an Ethernet driver that binds on the user's machine — a
  path that used to fail into a smoke log now fails in front of a person. It
  gets its own lane assertions.
- **The base system installs offline.** The ISO carries enough to produce a
  bootable console system with no network at all: kernel, base Debian, the
  overlay. A user with no cable still ends up with something that boots, and
  can add the desktop later with `apt install b1nix-desktop`. This is the
  difference between "needs network" and "useless without network", and it is
  worth the few hundred megabytes it costs.
- **The ISO is not 300 MB.** Calamares is Qt, so the installer environment
  carries a graphical stack; with the offline base included, the honest target
  is **900 MB–1.4 GB**, comfortably under the limit but nowhere near a classic
  netinst. The number is fixed at phase D, written into the lane, and a release
  that exceeds it either drops content or explains itself.
- **A mirror is a dependency at install time.** If `deb.debian.org` is
  unreachable, the desktop step fails. The installer treats that as a
  recoverable step, not a failed install: the base system is already on disk.

The size is enforced, not hoped for: `INSTALL-SMOKE` fails if the ISO exceeds
the budget, and the release checklist records the actual size.

## The desktop

- **Plasma on Wayland**, which is what the tree already drives, with an X11
  session installed as the fallback while the compositor path still has known
  bugs.
- **Plasma minimal, not `kde-standard`**: every extra application is another
  way to find a kernel bug in a release. The exact Depends/Recommends/Suggests
  split is in [packaging.md](packaging.md#b1nix-desktop).
- **Default applications**: a terminal, a file manager, a text editor, a
  browser, an image viewer, a settings app. A browser is the hardest consumer
  in the system and therefore the most valuable default — Firefox ESR from
  Debian, with Chromium a Suggests: the known-harder test rather than the
  default.
- **First boot**: a wizard for user, locale, keyboard, timezone, network. If
  Calamares already asked, the wizard does not ask again — on a live install it
  does nothing but show the known-issues page once.
- **Locale and input**: full UTF-8, the usual locales generated, a keyboard
  layout switcher configured by default. Anything less makes the distribution
  look like a demo.
- **Fonts**: a complete default set including Cyrillic and emoji. Missing
  glyphs are the most visible kind of unfinished.
- **Accessibility**: at minimum a working screen magnifier and high-contrast
  theme, because they are Plasma's and cost only testing. A screen reader
  (Orca) is a real kernel test too (speech-dispatcher, ALSA/PipeWire) and is
  worth listing as a goal even if it slips.
- **Audio**: PipeWire, as Debian ships it. The HDA path has history in this
  tree; sound that works on the first boot is a release gate.
- **Performance targets**, written down so regressions are visible: boot to
  login, login to usable desktop, and application start for the default
  browser, each recorded per release in the release notes.

## Hardware support

- **A hardware compatibility list** on the website, generated from real
  reports, with four states per machine: boots, installs, desktop works,
  daily-usable. Empty honesty beats an aspirational list.
- **`b1nix-report`** collects what a report needs into one plain-text file the
  user can read before sending. Fields, privacy rules and the list it feeds:
  [b1nix-report.md](b1nix-report.md).
- **Reference machines** that must work at every release, because they are the
  ones on hand: the Intel-graphics laptop, the UHD 620 laptop over PXE, the SM8150
  phone, and QEMU on both arches. Everything else is best-effort.
- **Firmware loading** through the standard paths so Debian's
  `firmware-*` packages work unmodified; `b1nix-firmware` only fills gaps.
- **The DKMS question**: `b1nix-kernel-headers` exists so that out-of-tree
  modules can be built, but nothing promises that Debian's DKMS packages
  (nvidia, virtualbox, zfs) compile against our kernel. The honest line is
  "the headers are there, the modules are not supported".

## Bug reporting

- **One issue template** that demands: the ISO or kernel version, the machine,
  what was expected, and a `b1nix-report` attachment. An issue without the
  report gets a bot comment, not a triage.
- **Serial and netconsole capture documented for users**, not just for
  development: a laptop owner with a USB-serial adapter or a second machine can
  produce the log that makes a panic fixable.
- **Crash triage in the tree**: the symbolized-backtrace path that development
  already uses, wired to the shipped kernel's build id, so a user's panic
  resolves to functions without a private build.
- **Labels that map to the roadmap**, so an issue can be closed as "fixed by
  M127" and the reporter can see when.

## Quality gates

The existing smoke suite is the backbone; the distribution adds lanes on top.

What a lane must look like — naming, stages, the known-degraded list — is in
[lanes.md](lanes.md).

- `DISTRO-SMOKE` — the installed image boots to a systemd target,
  `systemctl is-system-running` is `running` or a known `degraded` set,
  `apt update` works.
- `INSTALL-SMOKE` — unattended Calamares onto a blank disk in QEMU, then boot
  the installed disk to a login. Exercises Qt, udisks, parted, loop devices,
  GPT writes, LUKS and fsync — a hard kernel test disguised as a UX feature.
- `DESKTOP-SMOKE` — the existing graphics lanes, extended to: log in, open the
  browser, play a sound, suspend and resume once power management lands.
- `UPGRADE-SMOKE` — install release N, upgrade to N+1 through apt, reboot,
  still works. The lane that catches the mistakes that lose users.
- **Soak and fuzz** — the existing soak harness on the release candidate, plus
  a syscall fuzzer (syzkaller is the obvious import; even a crude one finds
  the first tier of bugs in a from-scratch kernel).
- **Hardware in the loop** — the UHD 620 laptop over PXE and the phone over fastboot, run
  manually at a release, with the checklist below.
- **The release checklist** lives in [release-checklist.md](release-checklist.md)
  and is mechanical: verify the lanes, the reference machines, the rescue recipe, the
  signature verification instructions, the known-issues list and the upgrade
  path from the previous release.

Two rules carry over unchanged and matter more here than in a kernel-only
project: no fake passes, and a marker is only emitted when the operation
really succeeded. A green lane that lies is worse in a shipped product than a
red one.

## Hosting and the repository URL

GitHub Pages and Releases, with no domain at first. The cost of that choice is
that the repo URL lands in strangers' `sources.list` and is painful to change
later, so it is contained:

- **The URL is never typed by a user.** `b1nix-base-files` ships
  `/etc/apt/sources.list.d/b1nix.list`; installing the distribution installs
  the source. Moving to a domain later is then a package update that ships
  through the old URL once, not an instruction everyone has to follow.
- **The ISO's install guide** points at the website, not at the raw repo path.
- **The build manifest and release notes** record the URL that was current, so
  an old ISO can be understood after a move.
- **Buy the domain before the first release that strangers use**, even if it
  only CNAMEs to Pages. Until then `<user>.github.io/b1nix` is the URL in every
  example in the docs.

## Release engineering

- **The pipeline** is specified in [ci.md](ci.md), including why the lanes may
  have to stay on this machine.
- **Built on the host** with `sbuild` in a pinned trixie chroot;
  `tools/deb/build-deb.sh` and `tools/deb/publish-repo.sh` are the
  only entry points, and both are runnable by hand and from CI.
- **CI on a tag** builds packages and ISOs for both arches, runs the lanes it
  can, signs, and publishes to Pages and Releases. CI cannot hold the offline
  key; the signature step either runs locally or uses a build-host subkey with
  a documented scope.
- **A build manifest** beside each ISO: every Debian package version that went
  in, the kernel git commit, the toolchain versions.
- **Size budget** for the ISO, stated and enforced by the lane — see
  [The image and its size](#the-image-and-its-size).
- **Artifacts per release**: amd64 ISO, arm64 phone image, `SHA256SUMS` + the
  signature, the build manifest, the release notes, the known-issues list.
- **Release notes** written from the git log but organised for a user: what is
  new, what is fixed, what is still broken, what to do before upgrading.

## Documentation and website

The website is the distribution's face and can be a handful of static pages:

- **Install guide** — verify the signature, write the USB, boot, Secure Boot
  note, run Calamares, first boot, how to get back if it does not boot. Drafted
  in [install-guide.md](install-guide.md).
- **Hardware support** — the compatibility list and how to add to it.
- **Known issues** — per release, honest, linked from the ISO's first boot.
- **FAQ** — what b1nix is, why a Debian userspace, what is actually ours, is it
  safe to use, can it replace Linux (no).
- **Developer docs** — the existing `docs/` topic guides, published rather than
  rewritten; the build instructions; how to run the smoke suite; how the
  linuxkpi shim works, because that is the interesting part of the project.
- **The architecture page** that explains the one thing that makes this project
  unusual: an original kernel that imports Linux drivers and filesystems
  through a shim, and runs a stock Debian on top.

## Community

- **Issue tracker** on GitHub, with the templates above.
- **A contribution guide**: [`CONTRIBUTING.md`](../../.github/CONTRIBUTING.md) — DCO
  sign-off, the commit-message rules already in the tree, how to run the lanes before sending a patch, what "no fake passes"
  means for a contributor.
- **A chat or forum** only when there is someone to answer in it; a dead
  channel reads worse than none.
- **No telemetry**, stated explicitly in the FAQ. The compatibility list is
  filled by voluntary `b1nix-report` submissions, which makes it smaller and
  trustworthy.
- **A public roadmap** — [roadmap.md](roadmap.md) for the distribution and
  [../kernel/roadmap.md](../kernel/roadmap.md) for the kernel; link both from
  the website so users can see where their bug sits.

## The phone

The Xperia 5 ships in the same release, with its own honest scope:

- **What it is**: a Debian arm64 root on b1nix, with SSH over USB, a console,
  and whatever display and input work at that release.
- **What it is not**: a phone. No cellular modem, no telephony stack, no
  battery-life claims. Saying so up front prevents the most predictable
  disappointment.
- **The path to more**: display and touch, then Plasma Mobile as the thing to
  aim at, then power management, in that order — each one a kernel milestone
  before it is a distribution feature.
- **Flashing** documented as carefully as the desktop install, including how to
  get back to the stock firmware. Bricking someone's phone is the worst
  outcome this project can produce.

## Phases

The phases, their contents and their status live in
[roadmap.md](roadmap.md), which is the source of truth for what is done:
A packaging skeleton, B bootable installed system, C cgroup v2 (M127),
D live ISO and installer, E public release 1 (Гнилиці), F arm64 in the same
release, G the second release and upgrades.

Two ordering constraints are worth repeating here, because they are what the
rest of this document rests on:

- **C before E.** systemd without cgroup v2 is a hollow init, so no ISO ships
  before M127 lands.
- **Boot counting before E.** The two-kernel fallback is the only thing standing
  between a bad kernel and a user who cannot boot, and with Limine it is code
  we write ourselves.

## Risks

| Risk | Why it is real | What blunts it |
|---|---|---|
| A bad kernel update leaves someone unbootable | A from-scratch kernel on unknown hardware | Two kernels, boot counting, the rescue recipe, snapshots |
| systemd exposes a hundred small ABI gaps at once | It touches nearly every subsystem | Phase C before the ISO; the Debian lane already runs systemd as PID 1 |
| Packaging work crowds out kernel work | Distributions are a bottomless time sink | 10–20 packages, everything else from Debian; one phase at a time |
| Nobody installs it | Enthusiast distributions are many | The kernel is the story, not the desktop; lead with it |
| A user loses data | Real, and the plan invites it | Say it once, plainly, in the install guide; make rollback work |
| Firmware or trademark misstep | Easy to do accidentally | The rules in [Licensing](#licensing-and-legal), checked at Phase A |
| Burnout | Evenings and weekends, a multi-year scope | Phases that each end in something shippable; nothing half-landed |

## What gets cut first

In order, when time runs out. Cutting from the top costs the least:

1. Accessibility beyond what Plasma gives for free.
2. Reproducible builds beyond the manifest.
3. The chat or forum.
4. The arm64 release riding the same tag (fall back to shipping the phone
   image separately).
5. `UPGRADE-SMOKE` automation (do it by hand at a release).
6. The live ISO's polish — the installer matters, the theme does not.
7. Fuzzing.

Never cut: signature verification instructions, the two-kernel fallback, the
known-issues list, no fake passes.

## Open decisions

- **Default filesystem** — btrfs buys snapshot rollback, which is the best
  protection against a bad kernel; ext4 is the better-tested path in this tree.
- **When to buy the domain** — the repo URL is a package setting until then
  (see [Hosting](#hosting-and-the-repository-url)), so this can wait, but not
  past the release where strangers start depending on the URL.
