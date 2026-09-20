# Repository layout

What lives where, why, and the move that gets the tree from today's shape to
this one. Nothing here is a matter of taste for its own sake: the present
layout groups files by where they came from, and the result is a root with ten
files in it, a `tools/` with thirteen subdirectories, and a `tests/programs/` that
holds three unrelated things and reads, to anyone new, like an abandoned
operating system. The layout below groups by **role** — what a file is for —
because that is the question someone actually asks when they open the tree.

## The shape

```
README.md  LICENSE  Makefile  CLAUDE.md          the only files in the root
.github/          SECURITY.md, CONTRIBUTING.md   (GitHub reads them here)

kernel/           the kernel. Unchanged.
  arch/ dev/ drm/ fs/ ipc/ lib/ lkpi/ mm/ module/ net/ sched/ syscall/
  user/ vdso/ bootinfo/ include/

tests/            everything that judges the kernel
  lanes/            the shell lanes (smoke.sh and the rest)
  programs/         the test binaries the lanes run (today tests/programs/bin)
  support/          data and helpers: mkgpt4k.py, dwc3-model/, 00-smoke.start,
                    known-degraded.txt, grade-index.py, verify-tone-wav.py,
                    fd-image.py, ppm-colours.py, the Linux ABI blob sources
  selfhost/         the proof that b1nix compiles its own kernel

tools/            everything that builds or runs something — five groups
  build/            the toolchain and the gates: compiler wrappers, build-musl,
                    build-toolchain, env.sh, kallsyms, module syms,
                    copy-if-changed, xxd-i, check-dynamic, check-rootfs-links,
                    check-b1cc-sync, unreferenced-scripts, and the allowlists
                    those checks read
  image/            making a bootable thing: mk-*.sh, mkiso.sh, push-kernel.sh,
                    the rootfs post-processing (prune/stamp/trim), the Alpine
                    staging with alpine.lock and alpine-ports.map, the asset
                    generators (TLS certs, CA bundle, test wav, b1cc initramfs),
                    applet-manifest.conf, openrc/, limine.conf.in, and the
                    rootfs overlay
  import/           fetching and staging foreign source: DRM core, i915 and its
                    firmware, the Linux filesystems, the shim header generator
  deb/              the distribution's packaging: debian-chroot, build-deb,
                    publish-repo
  run/              starting something and watching it: run-*.sh, run-distro.sh,
                    pxe-serve.sh, soak/, and the diagnostics (boot-timeline,
                    build-profile, netconsole-collect, limine-set-init)
  board/            per-machine work: the Xperia 5 scripts, the Raspberry Pi 4
                    firmware and hil.sh, rp2350

packaging/        the b1nix overlay's debian/ directories. Unchanged.
docs/             kernel/ and distro/ as they are now, plus licensing.md
third_party/      b1cc (the submodule), with its sysroot inside it
build/            a symlink to a volume with room for it. Unchanged.
```

## What moves, and what it costs

| From | To | Why |
|---|---|---|
| `LICENSING.md`, `THIRD_PARTY_NOTICES.md` | `docs/licensing.md` | one licensing document, not two in the root |
| `SECURITY.md`, `CONTRIBUTING.md` | `.github/` | GitHub reads them there, and the root stops growing |
| `tools/image/limine.conf.in` | `tools/image/` | it is an input to image building and nothing else |
| `third_party/b1cc-sysroot` | the b1cc submodule | it is b1cc's sysroot: its parser cannot read musl's headers. `tests/programs/Makefile` already says it "lives with the compiler that needs it" — it does not, yet |
| `tests/programs/bin` | `tests/programs` | 136 programs that assert syscall behaviour. They are tests |
| `tools/image/overlay` | `tools/image/overlay` | configuration of the Alpine smoke image |
| `tests/programs/compat`, `src/`, `libcxx_compat.c` | `tests/programs/support` | each is a dependency of a named test (m31, m32, m57, cxx_smoke) |
| `tools/configs/*` | with whoever reads them | `known-degraded.txt` to the lanes, the allowlists to the checks, `applet-manifest`/`openrc` to the image |
| `tests/support/00-smoke.start` | `tests/support` | it is guest content, not a tool |
| `tools/board/rpi4/firmware/*` | `tools/board/rpi4` | board firmware |
| `tools/import/drm`, `tools/import/fs` | `tools/import` | both fetch and stage imported Linux source |
| `tests/selfhost` | `tests/selfhost` | it proves a property of the kernel |
| `tools/run/debug/*` | `tools/run` | you reach for them while something is running |
| `smoke_run/` | stays | every lane and several documents name it; moving it buys a tidier root and costs a habit |

## What stays although it looks removable

Two things in this tree look like obvious deletions and are not. Both were
checked before writing this, and both have a reason recorded where someone
would find it only after removing them:

- **`tests/support/linux-abi/*.bin`** — prebuilt static Linux executables, committed on
  purpose so that building the kernel does not require a Linux-targeting
  compiler to be installed. The script that regenerates them says so in its
  header.
- **b1cc's sysroot** — deleting it as a "duplicate libc" has been tried; b1cc
  stopped compiling its own corpus, because its parser cannot read musl's
  headers. It moves to the b1cc repository in this plan; it does not disappear.

## Order of work

Three commits, cheapest first, each with a full build and a lane run — and,
because a rename cannot be judged by the build alone, a comparison of the
**number of checks** the suite reports before and after. A lane that quietly
stops asserting something is the failure mode of a move like this.

1. **The root.** `.github/`, the merged licensing document, `boot/` into
   `tools/image/`. A handful of references.
2. **`tools/`.** Thirteen directories become five, and the data that is not a
   tool leaves for `tests/support`. Around fifty references, nearly all in the
   Makefile.
3. **`tests/programs/`.** The name disappears: the sysroot goes to b1cc, the
   programs to `tests/programs`, the overlay to `tools/image/overlay`. The
   largest step, and the one that needs the b1cc repository to accept a commit
   first.

`git mv` throughout, so history follows the files.

## The rule this encodes

A directory is named for what its contents are **for**, not for where they came
from. `tools/image/alpine` held two package systems because both were packages;
`tools/image` held guest content because it was near an image. Grouping by
role is what makes "can we delete half of this?" answerable — the answer today
is no, and the reason is that the tree is mostly kernel and tests, which is the
right thing for it to be mostly made of.
