# bpkg — b1nix's native package manager

`bpkg` is a real userspace ELF (`userspace/bin/bpkg.c`, ~1000 lines of C11),
built and linked exactly like every other binary in the image (musl PIE,
`userspace/Makefile`'s generic `$(BUILD)/bin/%` rule, no special-casing). It
has no dependency on a ported zlib, libarchive, or curl: gzip/DEFLATE, tar,
and SHA-256 are all implemented from scratch inside the binary.

## Why a from-scratch decoder instead of the zlib port

The whole point of M104 is to shrink what the *main* b1nix repo needs built
from source before it can bootstrap a package manager. Linking `bpkg`
against the `tools/ports/build-zlib.sh` output would make the package
manager depend on the very from-source pipeline it exists to replace. RFC
1951 (DEFLATE) and RFC 1952 (gzip) are small, stable, unencumbered
specifications — a compliant decoder is a few hundred lines of C and, unlike
a vendored copy of zlib, never needs to track upstream.

## Repository formats

`bpkg` auto-detects which of two formats `INDEX_URL` points at:

1. **flat** — the house format already produced by
   `tools/packages/bpkg-publish.sh` and consumed at image-build time by
   `tools/packages/install-ports.sh`'s "download" mode:
   ```
   name version arch sha256 url [deps]
   ```
   `url` points at a single gzip-compressed tar of the package's files,
   rooted at `/`. sha256 is verified against the downloaded bytes before
   extraction (this was previously only checked by the host-side installer
   at image-build time — `bpkg` now does the same check at runtime).

2. **apk** — detected when `INDEX_URL` ends in `APKINDEX.tar.gz`, i.e. a
   real Alpine repository directory
   (`https://dl-cdn.alpinelinux.org/alpine/v3.20/main/x86_64/` and
   friends). `APKINDEX.tar.gz` is a gzip-compressed tar containing one text
   file, `APKINDEX`, with blank-line-separated records:
   ```
   P:pkgname
   V:pkgver
   A:arch
   D:dep1 dep2>=1.0 so:libfoo.so.1 !conflicting
   ...
   ```
   `bpkg` reads `P:`/`V:`/`A:`/`D:` (name, version, arch, dependencies) and
   builds each package's `.apk` URL as `<repo-dir>/<name>-<version>.apk`.
   Dependency tokens starting with `!` (conflicts), `/` (path-provides), or
   `so:`/`pc:`/`cmd:` (virtual/soname provides) are filtered out — they
   don't name an installable package in any index `bpkg` can resolve.

   A `.apk` file is Alpine's real container format: three **concatenated**
   gzip members (signature tarball, `.PKGINFO` control tarball, data
   tarball). `bpkg` decodes gzip members one at a time (`gunzip_one`,
   tracking the byte offset past each member's CRC32+ISIZE trailer) and
   extracts only the **last** member — the data tarball — as the package's
   files. The other two members are decoded (to walk past their length) but
   never written to disk.

## What's verified vs. what's an honest gap

**Verified**: a project-standard offline smoke module,
`userspace/rootfs-overlay/etc/bpkg-smoke.sh` (wired into
`tools/ports/00-smoke.start`'s `SMOKE_SYS` phase, checked by
`tests/smoke.sh` via `BPKG-SMOKE: ok *` markers), drives the real pipeline
against fixtures pre-staged in the rootfs:

- `pkgs/` — flat-format fixtures (`hello`, `dep1`, `needsdep`, `badpkg`)
  proving `update`/`install`/`list`/`remove`, sha256 checksum rejection
  (`badpkg` reuses `hello`'s tarball behind a deliberately wrong sha256 —
  install MUST fail and MUST NOT extract anything), and transitive
  dependency resolution (`needsdep` depends on `dep1`; both must land).
- `apkrepo/<arch>/` — a **real** `APKINDEX.tar.gz` + `.apk` pair, produced
  with the host's own `tar`/`gzip` (not `bpkg` itself), proving the apk
  parser/splitter against byte-for-byte real Alpine tooling output, not a
  format `bpkg` invented for itself.

Additionally, before this ever reached the target build, every hard piece —
DEFLATE, gzip framing, tar extraction (including the `./`-prefixed and
GNU-longname cases real packaging tools emit), the apk gzip-member splitter,
SHA-256, and `APKINDEX` parsing — was unit-tested on the host by compiling
`bpkg.c` directly (`#define main bpkg_main`) against the real fixture bytes
(committed test fixtures in `userspace/rootfs-overlay/pkgs/` and
`apkrepo/`), independent of the b1nix target toolchain. The binary itself
was also built and linked through the real `userspace/Makefile` pipeline
against musl (`x86_64-unknown-elf`, `ld.lld`, PT_INTERP
`/lib/ld-musl-x86_64.so.1`) — zero warnings.

**Not verified in this pass**: an actual QEMU boot of a rebuilt ISO. That
requires a from-scratch musl+LLVM toolchain build in this environment, which
was out of scope for the time available here. The `BPKG-SMOKE: ok
apk-format` marker exists and is wired into `tests/smoke.sh`, but nobody has
watched it print `ok` from inside a booted kernel yet — that is the next
concrete step before this can be called `done` rather than `initial`.

**Documented gaps, not silent shortcuts**:

- **No TLS.** `bpkg` speaks plain HTTP and `file://` only; an `https://` URL
  is rejected with an explicit error rather than silently downgraded or
  faked. Real Alpine mirrors and jsDelivr are HTTPS-only, so as shipped
  `bpkg` needs either an HTTP-capable mirror or a local/offline index. See
  "Adding TLS" below for the concrete follow-up.
- **No RSA signature verification.** `.apk`'s `.SIGN.RSA.*` member is
  decoded (to walk past it) but never checked against a trusted key — no
  public-key crypto is linked into this binary.
- **No `APKINDEX` `C:` (Q1 base64-sha1) per-file checksum verification.**
  Only the flat format's sha256 field is checked.
- **No chunked transfer-encoding.** Every target this ships against
  (jsDelivr, Alpine mirrors) sends `Content-Length` for static files.

## Usage

```
bpkg update                    # refresh /var/lib/bpkg/index from INDEX_URL
bpkg list                      # list installed packages + versions
bpkg search TERM                # search the cached index by name substring
bpkg info NAME                  # show cached index metadata for NAME
bpkg install NAME [NAME...]     # resolve deps, download, verify, extract
bpkg remove NAME                 # delete NAME's recorded files + metadata
```

Configuration lives in `/etc/bpkg.conf` (`INDEX_URL=...`, one KEY=VALUE per
line — the same file `tools/packages/install-ports.sh` already writes in
"download" mode). State lives in `/var/lib/bpkg/index` (cached index) and
`/var/lib/bpkg/installed/NAME.{list,ver}` (installed files + version) — this
is the *same* layout `install-ports.sh`'s download mode already produces, so
a rootfs seeded at image-build time is a rootfs `bpkg` can keep managing at
runtime, and `bpkg remove` can even undo an image-build-time install.

`BPKG_ROOT` (default `/`) overrides the extraction/removal root, and
`BPKG_INDEX_URL` overrides `/etc/bpkg.conf`'s `INDEX_URL` — both exist so
the smoke test (and any future sandboxed/offline install flow) can drive the
real pipeline against a scratch directory instead of the live rootfs,
without needing a second binary or a test-only code path.

## Adding TLS (concrete follow-up, not done here)

The cleanest path is *not* linking mbedTLS into `bpkg` directly (that would
reintroduce exactly the "ported C library baked into a boot-critical binary"
problem M104 exists to get away from). Instead: fetch has one call site
(`fetch_url` in `bpkg.c`) and mediates a request/response header/body split
in `http_fetch`; add an HTTPS transport as an alternative to `connect_host`
+ raw `read`/`write` gated on scheme, backed by mbedTLS's default in-tree
build (as `m32_nettool.c` already does under `B1NIX_HAVE_MBEDTLS`). That
keeps `bpkg`'s own logic (gzip/tar/sha256/index parsing) dependency-free and
isolates the one genuinely necessary third-party dependency (a TLS stack) to
a single, optional compile-time flag.
