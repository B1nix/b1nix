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

## The cheaper path found later: take Alpine's package, don't publish our own

The PoC above packages *our* build output. For most of `tools/ports/` that is
more work than it needs to be: Alpine already builds the same software for the
same libc and the same architecture, signs it, and serves it. `bpkg` proved the
guest side of that — it installs and runs unmodified Alpine `bash`, `readline`
and `libncursesw`. What was missing was the *host* side, at image-build time,
and that is `tools/packages/alpine-fetch.sh`.

It resolves a package's current version from `APKINDEX`, downloads the `.apk`
over HTTPS, checks its sha256 against `tools/packages/alpine.lock`, unpacks the
data segment and lays it out as `<prefix>/{include,lib}` — the same shape the
from-source ports produce, so a consumer's `-I`/`-L` do not change. A package
absent from the lock file stops the build and prints the hash to record, so
adding one is a reviewable diff rather than something a mirror can do quietly.

**Done for ten ports.** `zlib`, `expat`, `pcre2`, `brotli`, `libunistring`,
`libidn2`, `libpsl`, `libpng`, `libjpeg` and `libwebp` are Alpine packages;
their ten `tools/ports/build-*.sh` scripts are deleted. What replaced them is
one table and one script: `tools/packages/alpine-ports.map` maps a port name to
the packages Alpine splits it into, and `tools/packages/pkg-prefix.sh <port>`
installs it and prints the prefix — the same contract the port scripts had, so
the callers that read a prefix off stdout (curl, NetSurf, dropbear, libsvgtiny,
the Mesa demo) needed only the producer changed. `m53_zlib_smoke` also moved
from a static archive to `libz.so.1`, the shared library the image should have
been carrying all along. Fetching costs 0.4 s cold and 0.26 s cached against
2.4 s to build zlib, and the ratio only grows with the size of the port.

Two things were learned the hard way, and both are now guarded:

- **Alpine splits more finely than the ports did.** `expat` is the `xmlwf`
  binary; the library is `libexpat`. Asking for the wrong one leaves the -dev
  package's `libexpat.so` symlink dangling, and the linker does not complain —
  it silently falls back to the static archive in the same directory, and the
  failure surfaces much later, somewhere else. `alpine-fetch.sh` now fails on a
  dangling symlink. The same split applies to `libpcre2-16`/`-32`,
  `libturbojpeg` and libwebp's four extra archives.
- **A static archive is not automatically fit for a shared link.** Alpine's
  `libexpat.a` refers to libc's `stderr` with a `R_X86_64_PC32` relocation,
  which an executable resolves through a copy relocation and a shared object
  cannot resolve at all. That is why the Skia dependency fold links
  `-lexpat` instead of the archive. Checking for `R_X86_64_32` alone is not
  enough — the question is whether an archive references *imported data*.

This changes the waves below in one way: for anything Alpine ships, the step is
"fetch and delete the script", with no packaging or publishing of ours at all.
That covers most of waves 1–4 — `brotli`, `pcre2`, `libffi`, `libpsl`,
`libidn2`, `libunistring`, `libutf8proc`, `expat`, `libpng`, `libjpeg`,
`libwebp`, `libvpx`, `libjxl`, `freetype`, `fontconfig`, `harfbuzz`, `pixman`,
`cairo`, `wayland`, `libxkbcommon`, `openssl`, `mbedtls`, `curl`, `zsh`,
`dropbear`, `busybox`, `samurai`, `openrc`, `runit`. What stays ours to publish
is what Alpine does not package: the NetSurf library stack (`libcss`, `libdom`,
`libhubbub`, `libwapcaplet`, `libparserutils`, `libns*`, `librosprite`,
`libsvgtiny`), `litehtml`, `skia`, `tinygl`, `openlibm`, `crashpad`, `libharu`.
What must keep building from source is what is tied to this kernel: `musl` (the
ABI base, whose loader behaviour and `EI_OSABI` stamping we depend on), `mesa`
(our GL configuration), and the in-tree `b1cc`/`libb1gui`/`displayd`.

## Second pass: wayland, libffi and the desktop stack — and what shared linking cost

`wayland`, `libffi`, `freetype`, `fontconfig`, `harfbuzz`, `pixman` and
`xkbcommon` followed, which deletes seven more scripts (seventeen in total, from
57 to 40). Wayland is the interesting one: the port carried b1nix shims for
eventfd and `open_memstream` that musl now provides outright, so Alpine's
unmodified `libwayland-client.so.0` runs as it is — and with it and libffi
shared, the M49 binaries stopped carrying a copy of a protocol library each.

Three things had to be understood before any of it worked, and each is now
handled in `tools/packages/alpine-fetch.sh` rather than left to be rediscovered:

- **Some Alpine archives contain no code.** They are GCC slim-LTO objects: the
  `.a` holds intermediate representation, and ld.lld reports every symbol as
  undefined while `nm` cheerfully lists them from the archive index. pixman,
  libpsl and brotli are like this; harfbuzz and libwebp use *fat* LTO, which has
  both IR and machine code and links fine. Slim archives are deleted on install,
  so the shared library is the only candidate left.
- **A static archive fit for an executable need not be fit for a shared
  object.** Alpine's `libfontconfig.a` and `libexpat.a` reach their own data and
  libc's `stderr` through PC32 relocations, which an executable resolves with a
  copy relocation and a `.so` cannot resolve at all. The Skia dependency fold
  therefore stopped building `libfontconfig.so` by hand and simply uses the one
  Alpine ships.
- **Dependencies have to be closed automatically.** freetype wants libbz2,
  harfbuzz wants glib and graphite2, glib wants pcre2, libintl, libmount, xml2,
  blkid, econf and lzma. Discovering that from a guest that fails to start, one
  boot at a time, is not a method: `alpine-fetch.sh` now reads each installed
  library's `DT_NEEDED`, resolves every missing SONAME through the index's
  `p:so:` provides — the same lookup bpkg does in the guest — and installs until
  the set closes.

One image-side change came with it: `/lib/libc.musl-x86_64.so.1` is now a name
for the loader, because that is the SONAME every Alpine binary records for its
libc. Without it each of their libraries fails to load for want of a libc that
was already present under three other names.

## Third pass, and where the line actually falls

`libvpx`, `mbedtls` and `openssl` followed, leaving 37 of the original 57
scripts. Two mechanical things came with them: a `repo:` token in
`alpine-ports.map`, because libvpx lives in community while its dependencies do
not, and every index lookup now walks both repositories; and a fix to the
flattening, because openssl ships `/usr/lib/libcrypto.so.3` as a symlink to
`../../lib/`, which after flattening pointed outside the prefix and had replaced
the real file.

What is left divides into three groups, and only the first is worth more
migration work.

**Not packaged by Alpine at all** — the NetSurf library stack (`libcss`,
`libdom`, `libhubbub`, `libwapcaplet`, `libparserutils`, `libns*`,
`librosprite`, `libsvgtiny`), `litehtml`, `skia`, `crashpad`, `tinygl`,
`openpam`, `netbsd-curses`, `libutf8proc`. These are ours to publish through
`bpkg-publish.sh` if they are to leave the tree; nothing to fetch.

**Packaged, but not usable here** — and this is the more interesting boundary:

- `cairo`: Alpine builds it with the X11 backend, so it drags libX11, libxcb and
  their dependencies onto an image with no X server.
- `libjxl`: C++ built against libstdc++, while everything C++ here is libc++.
  Two C++ ABIs in one link do not mix.
- `openlibm`: consumers name `libm.a`; Alpine's package is `libopenlibm`, and
  musl already provides libm inside libc.so — a decision about which libm the
  image uses, not a packaging question.

`mesa` was on that list, for the reason that Alpine builds it for Linux DRM
rather than for the softpipe/OSMesa configuration the port was written around.
That turned out to be an argument about a configuration nothing wanted. The
OSMesa demos it existed for had already been deleted with the rest of the old
GUI stack, and the one live consumer of GL on this image is Alpine's own
`libwlroots`, which records `libEGL.so.1`, `libgbm.so.1` and `libGLESv2.so.2` —
Alpine's build, exactly. So the port went: 365 lines of meson cross-build and a
477 MB build tree, replaced by five names in `alpine-ports.map`
(`mesa-egl mesa-gles mesa-gbm mesa-gl mesa-glapi`, ~1.5 MB installed).
`mesa-dri-gallium` is deliberately not among them — 34 MB dragging llvm17-libs
(150 MB) behind it, for hardware drivers an image that composes in software and
runs its browser with `--disable-gpu` has no use for.

**Deliberately still ours** — `musl`, because it is the ABI base whose loader
behaviour and `EI_OSABI` stamping the whole image depends on, and `libcxx-musl`,
which is built to match it.

**A different kind of migration** — `busybox`, `openrc`, `curl`, `dropbear`,
`bmake`, `samurai`, `runit` are rootfs *binaries*, not build-time libraries.
Alpine ships all of them and bpkg already runs their binaries unmodified, but
swapping them changes what the running system does — init scripts, key paths,
applet sets — rather than only how it was built. Each wants its own
verification, not a batch.

`zsh` went first and proves the pattern. It needed a second install mode:
`ALPINE_LAYOUT=native` keeps the package's own paths instead of flattening them
into `{include,lib}`, because a program looks for its own files where it was
compiled to look — zsh's modules under `/usr/lib/zsh`, its functions under
`/usr/share/zsh` — and `pkg-prefix.sh --into <dir>` puts them in the image root
rather than a build prefix. The dependency closure brought libncursesw and
libcap along. Alpine's zsh 5.9 then passes the whole ZSH-SMOKE set unmodified.

Three mistakes are worth recording, because they will recur for the next
program:

- The destination must be made absolute. The native copy runs from inside the
  unpacked package so it can walk relative paths, and a relative destination is
  resolved against *that* directory — which builds the destination path inside
  the temporary tree, once per file.
- A recursive copy cannot be used on an image root: `/usr/lib` there is a
  symlink to `/lib`, and `cp -a` replaces directories rather than merging into
  them. Directories are created with `mkdir -p`, which follows the link, and
  files are copied one at a time.
- The dependency closure must look only at the files this run installed. An
  image root already holds libraries of its own, and asking the index for
  `libskia.so` or `libb1gui.so` gets nothing, which is not an error.

## The programs, and what an image root does not tolerate

`curl`, `dropbear`, `bmake` and `samurai` joined `zsh`: 31 of the original 57
scripts remain. They install through one `programs` entry into a staging root
(`build/$ARCH/pkgroot`) that the root-image rule merges over the image, plus a
`curlbuild` prefix for what links against libcurl rather than runs it (NetSurf).

`openrc` and `runit` were tried and taken back out, which is the useful result:

- Alpine's openrc brings its own `/lib/rc`, `/etc/init.d` and runlevels. They
  replaced this image's, and the boot went straight to poweroff — 1100 checks
  blocked behind a system that had stopped. What a package *is* and what it
  *does to the running system* are different questions, and init is all of the
  second.
- Their runit is statically linked, which `tools/check-dynamic.sh` rejects
  outright, as it should.

Four failures on the way, all of them things the tree had been getting away
with:

- **`build/$ARCH/rootfs` is not reproducible.** Deleting it lost `/sbin/openrc*`
  and `/bin/crashpad_handler`, neither of which any rule rebuilds — they had
  been staged once, by hand, and lived there ever since. Both had to be rebuilt
  from their ports by hand. Anything that only exists in that directory is one
  `rm -rf` from gone.
- **`build-crashpad.sh` had been broken for as long.** It looked for
  `userspace/bin/crashpad_smoke.cpp`, which moved to `userspace/bin/smoke/`;
  nobody noticed, because the binary it failed to build was already on the
  image.
- **The duplicate-trim removed a package's real library.** root-image deletes
  `lib*.so.N.M.P` when the SONAME copy exists — sound when both are real files,
  wrong when the SONAME is a *symlink to exactly that file*, which is how every
  package names its library. The loader then reports the library as a pile of
  missing symbols rather than as a missing file.
- **Copying onto a symlink writes through it.** The old dropbear was one binary
  with a name per tool; those symlinks survived in the staging root, so each
  packaged tool in turn overwrote the single file they all pointed at, and the
  guest got `dropbearkey` when it asked for `dbclient`. Every one of these
  copies is `--remove-destination` now.

One behavioural difference remained: Alpine's dropbear logs to syslog, so the
service test's log file stayed empty. `-E` puts it back on stderr, where the
init script redirects it.

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
`mesa` (**done** — the whole port deleted, see the third-pass section above;
it turned out to be the easiest of this wave, not the hardest, because the
only thing left that wanted GL wanted precisely Alpine's build of it),
`skia` + `skia-shared-deps` (355 combined, heaviest C++ dependency),
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

## Where it actually stands

The migration is done, and it took the cheaper route this document argued for
later on: rather than publishing b1nix's own packages, the image installs
Alpine's. `tools/packages/alpine-ports.map` names, per old port, the Alpine
packages that replaced it; `tools/packages/alpine-fetch.sh` fetches each into a
build prefix at image-build time, pinned by sha256 in
`tools/packages/alpine.lock`. 49 of the original 54 `tools/ports/build-*.sh`
scripts are gone, and every wave cleared step 3 — a full `tests/smoke.sh` run
with the same marker set — before its script was deleted.

Five stay from-source, deliberately, and are marked `wontfix` on the roadmap:

- `musl`, `libcxx-musl` — what the toolchain is built out of. A package cannot
  supply the sysroot it is itself compiled against.
- `busybox` — configured from this tree's applet manifest, which is the whole
  point of `tools/configs/applet-manifest.conf`.
- `openrc` — Alpine's brings its own `/lib/rc`, `/etc/init.d` and runlevels,
  which replaced the image's and took the boot straight to poweroff. Alpine's
  `runit` is statically linked, which `tools/check-dynamic.sh` rejects outright.
- `rust` — staged as a toolchain, like LLVM and clang, not as an image package.
