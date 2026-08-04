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

## Transport: HTTPS, and what it verifies

`bpkg` links mbedTLS (the same static archives `build-curl.sh` already
produces) behind `B1NIX_HAVE_MBEDTLS`, so `https://` URLs work. The
certificate chain is checked with `MBEDTLS_SSL_VERIFY_REQUIRED` against
`/etc/ssl/certs/ca-certificates.crt`, and the hostname is passed through
`mbedtls_ssl_set_hostname()` (SNI *and* the name the certificate must
match). A missing trust store is a hard failure, not a downgrade.

The bundle is copied into the image from the **build host's** own store
(`/etc/ssl/cert.pem`, `/etc/ssl/certs/ca-certificates.crt`, or
`$B1NIX_CA_BUNDLE`) rather than vendored into git, so the image never ships
a CA list that quietly goes stale. Everything above the transport — gzip,
tar, sha256, index parsing — stays dependency-free; TLS is confined to
`conn_open`/`conn_read`/`conn_write`.

## Package authenticity

A real `.apk` carries a chain, and `bpkg` checks all of it:

| gzip member | contents | checked how |
|---|---|---|
| 1 | `.SIGN.RSA.<key>` | RSA PKCS#1 v1.5 / SHA-1 over the **stored bytes** of member 2, against `/etc/apk/keys/<key>` |
| 2 | `.PKGINFO` | carries `datahash` — sha256 of the stored bytes of member 3 |
| 3 | the package's files | sha256 must equal that `datahash` before anything is extracted |

Verifying only the signature would leave the payload unauthenticated, so
both links are required. The trusted keys are Alpine's own public keys,
shipped in the image at `/etc/apk/keys/` (public halves only, from the
`alpine-keys` package).

Policy, stated rather than implied:

- signed package, signature verifies → install
- signed package, bad signature, unknown signer, or `datahash` mismatch → refuse
- unsigned package over `file://` → install (the offline smoke fixtures)
- unsigned package over the network → refuse
- signed package but a `bpkg` built without mbedTLS → refuse (it cannot check)

## Multiple repositories, and virtual dependencies

`INDEX_URL` takes a space-separated list; Alpine splits its packages across
`main/` and `community/`, and dependency resolution needs both in one set.
Each repository is cached separately (`/var/lib/bpkg/index`, `index.1`, …)
and all of them parse into one package set.

Alpine states most dependencies as virtual tokens — bash depends on
`so:libreadline.so.8` and `/bin/sh`, not on "readline". `bpkg` builds a
provides table from the index's `p:` lines and resolves through it, so the
real dependency graph resolves (bash → readline → libncursesw → …). Tokens
the base image already satisfies are listed in `/etc/bpkg.provided`
(`musl`, `so:libc.musl-x86_64.so.1`, `busybox`, `/bin/sh`, …) — without it,
installing anything real would pull Alpine's own musl and BusyBox over the
running system's libc, loader and shell. A token that is in neither the
provides table nor `bpkg.provided` is reported as unresolved.

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

**Documented gaps, not silent shortcuts**:

- **No `APKINDEX` `C:` (Q1 base64-sha1) per-file checksum verification.**
  The signature chain (signature -> control -> `datahash` -> payload) covers
  the package bytes, so this field adds nothing the chain does not already
  prove; it stays unread.
- **No chunked transfer-encoding.** Every target this ships against
  (jsDelivr, Alpine mirrors) sends `Content-Length` for static files.
- **No `.post-install` / `.pre-install` scripts, no triggers, no
  `/etc/apk/world`.** A real `.apk` may carry them; `bpkg` extracts the
  payload and records the file list, nothing more. Packages that need a
  post-install step are installed incompletely — that is a gap, not a
  silent success, and it is the next thing to build before migrating the
  from-source ports to packages.
- **No file-conflict detection between packages.** Two packages owning the
  same path both write it; last one wins.

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

## Running real Alpine binaries

b1nix builds musl 1.2.5, the same release Alpine v3.20 ships, and loads
`/lib/ld-musl-x86_64.so.1` PIEs already — so Alpine's own x86_64 packages run
unmodified, with no rebuild and no patched `EI_OSABI` byte (the loader treats
any binary requesting the musl interpreter as Linux-personality, see
`elf64_is_linux_binary` in `kernel/user/process.c`).

Proving that took three kernel fixes, all of which were general defects rather
than anything to do with packaging:

1. **`/dev/fd`, `/dev/stdin|stdout|stderr` did not exist**, and
   `/proc/self/fd/<N>` was an ordinary symlink to text like `pipe:[63]`. On
   Linux it is a *magic link*: opening it returns another reference to the same
   open file description. bash's process substitution (`cmd < <(other)`) hands
   the reader `/dev/fd/63`, so without both pieces the open failed with ENOENT,
   nothing ever read that pipe, and the writer blocked in `pipe_write()` until
   the entire pipeline (and the boot behind it) wedged.
2. **A datagram socket sent with source port 0** unless it had been explicitly
   bound to a nonzero port, so every reply came back to a port no socket
   claimed. `bind(port=0)` means "any port", not "port zero".
3. **`recvfrom` reported the socket's last send target and `recvmsg`
   zero-filled `msg_name`.** musl's resolver uses `recvmsg` and discards any
   reply whose source does not match the nameserver it queried — so DNS failed
   system-wide with the correct answer sitting in the buffer.
4. **Pipes held 4 KiB, Linux's hold 64 KiB.** bash sends a here-document
   through a pipe when it believes the document fits, so neofetch's
   `read -rd '' config` (a ~10 KiB here-doc) filled the pipe and blocked
   forever — no error, no output, just a wedged process that even SIGTERM
   could not reach because it never returned from the write. The buffer is now
   64 KiB, allocated per pool slot on first use.

With those in place, `bpkg install neofetch figlet` against
`dl-cdn.alpinelinux.org` resolves the name, fetches over HTTPS, pulls
`bash`, `readline`, `libncursesw` and `ncurses-terminfo-base` through the
`so:` provides graph, verifies every signature and datahash, and installs.
`figlet` — a plain C binary from the same mirror — then runs and prints its
banner.

neofetch itself starts, reads its config, runs `uname -srm` and stops at its
own OS table:

```
Unknown OS detected: 'B1NIX', aborting...
Open an issue on GitHub to add support for your OS.
```

That is neofetch's whitelist, not a defect here — bash executes the whole
script up to that `case`. Patching the package to recognise b1nix, or
teaching neofetch's OS detection about it upstream, is the only way past it,
and neither belongs in the package manager.
