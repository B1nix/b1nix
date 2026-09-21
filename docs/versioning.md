# Versioning

Four numbers, four owners. They move independently, and only one of them is
written by hand.

| Number | Where it lives | Who reads it | Who bumps it |
|---|---|---|---|
| Kernel version | `B1NIX_VERSION_STR` in `kernel/include/b1nix/version.h` | `uname -r`, `/lib/modules/<release>`, module vermagic, `/proc/version` | a human, at a release |
| Linux ABI claim | `B1NIX_LINUX_ABI_RELEASE`, same file | glibc's `ld.so` against `NT_GNU_ABI_TAG`; anything that feature-tests by release | a human, when the ABI layer really grew |
| Distribution release | `b1nix-base-files` → `/etc/os-release` | the person downloading the ISO | a human, at a distribution release |
| Package version | `debian/changelog` of each overlay package | `apt`, `dpkg` | a script, derived |

## Kernel version — `0.MINOR.PATCH`

Until 0.123.0 the minor tracked the roadmap milestone number. It had already
drifted (0.123.0 while M124 was the closed milestone), and a milestone counter
cannot express two releases inside one milestone or two milestones in flight.
From 0.124.0 the coupling is dropped. Old tags stay as they are; no number is
rewritten.

- **MINOR** — observable behaviour changed: a new syscall or subsystem, a
  changed default, a driver that now binds, a performance change big enough
  that someone would notice.
- **PATCH** — fixes and internal work behind an unchanged surface.
- **1.0.0** — cut with distribution release 1: the ISO installs through
  Calamares onto a disk and the installed system boots to a desktop with a
  working `apt`. Before that, everything is 0.x and nothing is promised.

The milestone number lives in `docs/kernel/roadmap.md` and nowhere else. A release
commit is `build: version 0.127.0`, and every bump gets a tag `v0.127.0` — the
tag is part of the bump, not an afterthought, because the Debian package
version is derived from `git describe`.

## Linux ABI claim — `B1NIX_LINUX_ABI_RELEASE`

An independent knob, and it stays independent. It is a claim about what the
Linux ABI layer implements, not about b1nix's own maturity, and glibc refuses
to start binaries under a release older than their `NT_GNU_ABI_TAG` minimum.
Raise it only when the layer genuinely gained what the newer release implies —
cgroup v2 and io_uring are the two that will justify the next raise — and say
in the commit body which features earned it.

`B1NIX_RELEASE_STR` composes the two and needs no rule of its own:

```
uname -r  →  6.6.0-b1nix-0.127.0
```

## Distribution release — a counter and a codename

`b1nix 1 "Гнилиці"`. Not a date, not the kernel version: the ISO ships when
a phase is finished, and a date would imply a cadence that evenings and
weekends do not support.

- The counter increments on every ISO that is announced: 1, 2, 3.
- A respin of the same release — a rebuilt ISO with fixes, no new phase — is
  `1.1`, and keeps the codename.
- The codename is fixed at the release and never reused. The theme is amusing
  Ukrainian village names; release 1 is Гнилиці, transliterated `hnylytsi` in
  every file that needs ASCII.

```
b1nix-1-amd64.iso
b1nix-1-arm64-bahamut.img

/etc/os-release:
  NAME="b1nix"
  PRETTY_NAME="b1nix 1 (Hnylytsi)"
  VERSION="1 (Hnylytsi)"
  VERSION_ID="1"
  VERSION_CODENAME=hnylytsi
  ID=b1nix
  ID_LIKE=debian
  B1NIX_KERNEL="0.124.0"
```

`ID_LIKE=debian` matters: third-party install scripts branch on it, and
lying about `ID` instead would break `apt`-aware tooling.

The distribution release is tagged `b1nix-1` — a separate tag namespace from
the kernel's `v0.124.0`, because the two move at different speeds.

## Package versions — derived, never typed

Overlay packages take their version from the kernel version and the git
state, so `apt` always sees a monotonic string:

- On a tag: `0.124.0-1`
- Between tags: `0.124.0+git20260920.1493.841bf528-1`

`0.124.0+git…` sorts above `0.124.0` and below `0.124.1`, which is what a
snapshot should do. The number between the date and the hash is the commit
count, and it is not decoration: two snapshots from the same day are otherwise
ordered by their hashes, which are not monotonic, so the newer build can sort
below the older one and the repository will refuse it. The `-1` is the Debian revision: it increments when the
packaging changes and the upstream version does not.

`tools/deb/build-deb.sh` generates the `debian/changelog` entry from
`git describe --tags --long`; a hand-written version in a changelog is a bug.
Packages that are not versioned with the kernel (`b1cc`, `b1nix-artwork`)
carry their own upstream version and the same revision rule.

## Where the plan lives

The distribution's releases and what goes in them: [distro/roadmap.md](distro/roadmap.md).
The kernel's milestones: [kernel/roadmap.md](kernel/roadmap.md).

## Rules of thumb

- Never rewrite a released number, even a wrong one. Fix the scheme going
  forward and write down what changed, as this file does for the 0.123 drift.
- One knob per question. If a number has to answer "how new is this" and "what
  does it support" at once, split it.
- A version bump that is not tagged did not happen.
- `B1NIX_VERSION_STR` is not bumped in ordinary commits — only when a release
  is cut or the user asks.
