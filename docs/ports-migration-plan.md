# Ports-out-of-tree migration plan

Goal (per the M104 task): the *main* b1nix repo ends up containing only
`kernel/` and `userspace/` — the OS itself and the minimal libc/crt/core
binaries needed to bootstrap — with every third-party port currently built
from source by `tools/ports/*.sh` distributed instead as a prebuilt package
`bpkg` (see `docs/bpkg-package-manager.md`) can install.

This is explicitly **not** done in one shot. Ground rules from the task (and
honored here): investigate, plan concretely, prove the mechanics work on one
simple case, and do not delete any `tools/ports/*.sh` script until the new
flow is proven end-to-end for that specific port, including a real QEMU boot
verifying the installed artifact actually works.

## What exists today

- `tools/ports/*.sh` (54 scripts, ~5,800 lines) — each downloads an upstream
  tarball and cross-compiles it against the musl/ld-musl toolchain, on every
  fresh checkout, into `build/$ARCH/ports/<name>/install/`.
- `tools/packages/bpkg-publish.sh` — already takes a built directory tree,
  tars it, computes its sha256, and appends a line to a flat-text
  `pkgs/index` (`name version arch sha256 url [deps]`). It does not push
  anywhere; that's a manual `git push` to a separate public repo
  (`B1nix/b1nix-pkgs`) served over jsDelivr.
- `tools/packages/bpkg-build-all.sh` — a manifest-driven wrapper that packs
  whatever's present in a *built* rootfs (`dev`, `zsh`, `b1cc`, `curl`,
  `dropbear`, `netsurf`, ...) via `bpkg-publish.sh`.
- `tools/packages/install-ports.sh` — the *consumer*, called from the
  top-level `Makefile`'s `install-ports` target. In `download` mode it pulls
  every arch-matching entry from `pkgs/index` into a rootfs at image-build
  time (host-side, via `curl`/`tar`, no in-guest step). In `local` mode it
  still shells out to `tools/ports/build-*.sh` for a handful of packages
  (zsh/curl/dropbear/netsurf) that the published index doesn't cover yet or
  that the Makefile wants freshly built (see `overlay_local_ports` in that
  script — it prefers a locally-built binary over the published one for
  several packages that predate the musl migration).
- `tools/packages/stage-toolchains.sh` — the existing precedent for "big
  binary, doesn't fit jsDelivr, ships via GitHub Releases instead": LLVM,
  clang, and rustc are already distributed this way, not built from source
  by the main repo on every checkout.

So the pieces already exist. What's missing is (a) `bpkg`, the in-guest
consumer that can *also* run at runtime instead of only at host image-build
time (built in this pass — see `docs/bpkg-package-manager.md`), and (b) a
decision + concrete plan for actually retiring `tools/ports/*.sh` in favor
of this pipeline.

## Proof of concept done in this pass

Rather than fabricate a synthetic example, the PoC packages a **real** build
artifact already produced by `tools/ports/build-zlib.sh` in this
environment (`build/x86_64/ports/zlib/install/{lib/libz.a,include/*.h}`):

1. `tools/packages/bpkg-publish.sh <zlib-install-dir> zlib 1.3.1 x86_64` —
   produced a real gzip tarball, a real sha256
   (`11dd86e4c192d4e73c35a533d1469dd9e4424959ad79f581a7d93e93518e377d`), and
   a real flat-index line, using the *existing, unmodified* publish script.
2. Hosted the tarball behind a `file://` URL (standing in for a real HTTP
   mirror — `bpkg` doesn't care which, see "no TLS" caveat below).
3. Ran `bpkg`'s own index-fetch → sha256-verify → gunzip → tar-extract path
   (exercised at the C function level, since a full QEMU rebuild wasn't
   feasible in this pass — see `docs/bpkg-package-manager.md` for exactly
   what was and wasn't run inside the kernel) against that URL.
4. Diffed the extracted `lib/libz.a`, `include/zlib.h`, `include/zconf.h`
   against the original build output: **byte-for-byte identical.**

This proves the mechanical chain end to end: a port's build output, once
produced anywhere (a developer machine, CI), can be packaged once with the
*existing, unmodified* `bpkg-publish.sh` and installed on any number of
b1nix rootfs images afterward with zero recompilation — which is the entire
point of getting these ports out of the main repo's from-source path.

**What this PoC does not yet cover** (the honest remainder): `bpkg install
zlib` was not run inside a *booted b1nix kernel*, and no consumer binary was
rebuilt against the bpkg-installed `libz.a` to confirm it links/runs
correctly from that location. `tools/ports/build-zlib.sh` has **not** been
deleted or modified — nothing here is destructive.

## Migration waves — concrete, ordered by blast radius

Grouped by dependency depth and risk, not just source size. Each wave should
land as: package the wave's ports (once, via `bpkg-build-all.sh` /
`bpkg-publish.sh`, unchanged) → update `install-ports.sh`'s flat index (or
switch it to consume a proper `APKINDEX`-shaped repo, see below) → run the
full `tests/smoke.sh` suite against a rootfs built via `download` mode only
→ only then remove that wave's `tools/ports/build-*.sh` scripts.

### Wave 0 — already effectively out-of-tree (precedent, not new work)
LLVM, clang, rustc via `stage-toolchains.sh` + GitHub Releases. Nothing to
do here except point at it as the existing precedent for "big/slow to
build, ship prebuilt."

### Wave 1 — leaf libraries, no interdependencies, small (lowest risk)
`zlib`, `brotli`, `libutf8proc`, `libunistring`, `pcre2`, `libffi`,
`libpsl`, `libidn2`, `openlibm`, `libwapcaplet`, `libnsutils`, `libnspsl`,
`libnslog`, `librosprite`. All are single-artifact static/shared libraries
under ~100 lines of build script, consumed by exactly one or two downstream
ports each. Failure mode if something's wrong: one smoke test category
fails, easy to bisect. **Do this wave first** — it's also the largest single
batch by count, so it proves the *pipeline* (packaging → index → install →
smoke) scales past a single hand-picked example before anything riskier
rides on it.

### Wave 2 — small self-contained apps/tools
`busybox`, `zsh`, `dropbear`, `curl`, `mbedtls`, `openssl`, `bmake`,
`samurai`, `netbsd-curses`. These produce a handful of binaries each with
few build-time deps (mbedtls/openssl are curl's TLS backend and should move
together with curl in the same wave). `bpkg-build-all.sh`'s manifest already
covers `zsh`/`curl`/`dropbear` — extending it to the rest of this wave is
mechanical.

### Wave 3 — image codecs and small format libraries
`libpng`, `libjpeg`, `libwebp`, `libvpx`, `libjxl`, `expat`, `libharu`.
Self-contained, but several are dependencies of Wave 4's desktop stack, so
they need to land (and be smoke-verified) before Wave 4 starts.

### Wave 4 — desktop/text stack (moderate risk, real interdependencies)
`freetype`, `fontconfig`, `harfbuzz`, `pixman`, `cairo`, `xkbcommon`,
`wayland`, `libcxx-musl`. These depend on each other and on Wave 3's codecs;
`fontconfig` also needs a populated font cache convention that currently
gets baked in at image-build time (see `install-ports.sh`'s NetSurf-asset
staging for the shape of the problem: some ports need *runtime resource
files* alongside the binary, not just the binary). `bpkg`'s flat format
already supports this — the package's tarball just needs to include those
resource paths — but the manifest/build step needs updating per-port.

### Wave 5 — NetSurf's own dependency stack
`libdom`, `libcss`, `libhubbub`, `libparserutils`, `libsvgtiny`, `libnsgif`,
`libnsbmp`, `libnsfb`, `litehtml`. Small individually but numerous and only
useful together; package as one logical group (they should probably become
one `bpkg` package — `netsurf-libs` — rather than nine, to avoid nine
near-simultaneous dependency edges that all break the same way if one is
wrong).

### Wave 6 — high risk: large, slow, easy to get subtly wrong
`netsurf-fb` (834-line build script, the single biggest port script and
already flagged in `install-ports.sh`'s own comments as needing a "fresher"
local build over the published one due to past musl-migration staleness),
`mesa` (355 lines, GPU-facing, already has documented render-verification
gaps per the M52 project memory — "render unverified, too slow under
TCG"), `skia` + `skia-shared-deps` (355 combined, heaviest C++ dependency),
`runit`/`openrc` (init systems — getting these wrong means the image
doesn't boot, not just "a feature is missing"), `busybox` is listed in Wave
2 but note `openrc`/`runit` specifically must be smoke-tested with `-smp 4`
too, since M24b/M28 already found real SMP bugs in the userspace-execution
path that a single-CPU smoke pass wouldn't catch.

**Recommended order within Wave 6**: `netsurf-fb` and `mesa` before
`runit`/`openrc`. A broken NetSurf or Mesa package produces a visible,
isolated smoke failure (`M53-WEB`, `M52-GFX`, etc.); a broken init-system
package produces a kernel that boots to nothing, which is a much harder
failure mode to bisect from a `tests/smoke.sh` timeout alone.

### musl itself — deliberately not in any numbered wave
`build-musl.sh` cross-compiles the *sysroot every other userspace binary
links against* (`Scrt1.o`, `crti.o`/`crtn.o`, `libc.so`, headers). It is not
a "port" in the same sense as the others — it's part of the toolchain, like
LLVM/clang/rustc in Wave 0. It could eventually follow the same
`stage-toolchains.sh` + GitHub Releases path (the sysroot is ~28 MB,
well within a Release asset budget, unlike jsDelivr's per-file limits), but
it should be the **last** thing migrated, well after every wave above has
proven the packaging pipeline is trustworthy — a bad musl package breaks
every other package's ability to link, not just one.

## How the separate build/CI repo should work

`tools/packages/bpkg-publish.sh` and `bpkg-build-all.sh` already assume this
shape and need no changes to serve it:

1. A **new** repo (not this one) — call it `b1nix-ports-build` — carries the
   `tools/ports/*.sh` scripts once they're retired from here, plus a CI
   workflow (GitHub Actions) that, on a schedule or on a port-version bump,
   runs each `build-*.sh`, then `bpkg-build-all.sh` against the resulting
   rootfs, and commits the produced `pkgs/` tree to `b1nix-pkgs` (or pushes
   large artifacts to a Release, per `stage-toolchains.sh`'s existing
   pattern).
2. `b1nix-pkgs` stays exactly what it is today: a public repo whose `pkgs/`
   directory jsDelivr serves. No format change needed for the flat index;
   `bpkg` already speaks it.
3. **Optional, larger change**: instead of (or alongside) the flat index,
   have the CI repo also emit a real `APKINDEX.tar.gz` + `.apk`-shaped
   layout per architecture directory (`pkgs/x86_64/APKINDEX.tar.gz` +
   `pkgs/x86_64/<name>-<version>.apk`). `bpkg` already speaks this format
   (verified against real Alpine-shaped fixtures — see
   `docs/bpkg-package-manager.md`). The advantage: standard Alpine tooling
   (`abuild`, `apk`) could inspect/mirror it, and it's a well-understood
   format for anyone auditing the pipeline. The flat format's only
   real advantage is that it's already what `install-ports.sh` consumes
   today, so switching only helps if the CI repo also wants to serve real
   third-party Alpine packages (musl-linked Alpine binaries the task's
   framing already established b1nix's syscall layer can run) alongside
   b1nix's own ports — worth doing, but a separate decision from "get the
   from-source builds out of the main repo," not a prerequisite for it.
4. The main repo's `Makefile` `install-ports` target already defaults to
   `download` mode (`tools/packages/install-ports.sh $(BUILD_DIR)/rootfs
   $(ARCH) download $(PACKAGE_INDEX_URL)`) — each wave's migration is
   mechanically "delete the wave's `local`-mode fallback paths in
   `install-ports.sh` (`local_ports`, `overlay_local_ports`,
   `overlay_local_dropbear`) once the published package is proven
   equivalent," not a Makefile rewrite.

## Makefile changes needed (main repo)

- `install-ports` target: unchanged in shape, but as each wave retires its
  `tools/ports/build-*.sh` scripts, remove the corresponding prerequisite
  edges that currently force a local build (search the top-level `Makefile`
  for targets that `foreach`-invoke `tools/ports/build-*.sh` directly, not
  through `install-ports.sh`, e.g. anything gating `iso-full`/desktop
  targets on a local Mesa/NetSurf build).
- `check-tools`: currently checks for build prerequisites (cross compiler,
  `mke2fs`, etc.) for ports built from source; once a wave no longer builds
  from source, its host-side prerequisites (autoconf/automake/meson for
  that specific port, if nothing else needs them) can drop out of that
  check.
- New target, e.g. `install-ports-refresh`: force a `bpkg update` +
  reinstall of every package on an already-built rootfs, for iterating on
  package content without rebuilding the ISO from scratch — useful once
  most ports are prebuilt, since the edit-compile-test loop shifts from
  "rebuild the ISO" to "swap a package."

## What must be true before deleting any `tools/ports/build-*.sh`

Per the task's own ground rules, restated as a concrete checklist per port
(or per wave, for the small leaf libraries):

1. `bpkg-build-all.sh` (or `bpkg-publish.sh` directly) produces a package
   from the port's last-known-good build output.
2. That package is hosted (locally for dev iteration, then on `b1nix-pkgs`
   for real) and installs via `bpkg install <name>` — or, for image-build
   time, via `install-ports.sh download` mode — into a clean rootfs.
3. A full `tests/smoke.sh` run (both `ARCH=x86_64` and `ARCH=x86`, both
   single-CPU and `-smp 4` per the project's SMP-bug history) against that
   rootfs passes with the SAME marker set as a from-source build produced.
4. Only then: delete that port's `tools/ports/build-*.sh` and remove its
   `local`-mode fallback in `install-ports.sh`.

Nothing in this pass reached step 3 for any real port (see
`docs/bpkg-package-manager.md` for exactly what stopped short and why —
mainly: a full ISO/QEMU cycle needs a from-scratch toolchain build this
environment didn't have time for). The zlib PoC above reached step 2 for a
real artifact. That is the honest state of part 2 of this task: a concrete,
ordered plan plus a verified-as-far-as-time-allowed proof of the mechanism,
not a completed migration.
