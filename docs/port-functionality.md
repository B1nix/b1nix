# Third-Party Port Feature Enablement

This document tracks the per-port features that were disabled when each upstream
project was first ported (to get it building at all), which ones have since been
**enabled** as the OS gained the required syscalls/libc APIs, and the remaining
**milestones** for full feature parity.

Rule of thumb: a port feature is only flipped on once the underlying kernel/libc
capability is real (no fake markers), and every flip is re-verified by the full
smoke suite on **both** arches.

> **Smoke caveat:** the desktop-graphics tests (`M51` clipboard, `M52` GLSL,
> `M53-WL` windowed browser) are sensitive to host load. Running the smoke suite
> while other heavy builds are in flight produces spurious failures; always smoke
> on a quiet host.

## Enabled (verified both arches — x86 727/0, x86_64 728/0)

| Port | Feature turned on | Backed by |
|---|---|---|
| **libc/kernel** | timed futex → `pthread_cond_timedwait`, `pthread_mutex_timedlock`, atomic+futex semaphores (`sem_timedwait`/`trywait`/`getvalue`) | `SYS_FUTEX` timeout (`scheduler_block_on_timeout`) |
| **libc/kernel** | `settimeofday`/`clock_settime` | `SYS_SETTIMEOFDAY` → `rtc_set_unix_time` |
| **libc/kernel** | `sched_getaffinity` + `cpu_set_t`/`CPU_*` | `SYS_SCHED_GETAFFINITY` (online-CPU mask) |
| **libc** | `scandir`/`alphasort`, POSIX `remove()` | opendir/readdir + qsort |
| **toolchain** | `--enable-threads` works for any autotools port | `b1nix-autotools-cc` drops libc-provided `-l` names |
| **curl** | `file://`, unix-sockets, alt-svc, HSTS, WebSockets, headers-api, dateparse, threaded resolver | sockets + pthreads |
| **mbedTLS** | `MBEDTLS_HAVE_TIME` + `HAVE_TIME_DATE` (cert notBefore/notAfter validation) | settable wall clock + `clock_gettime` |
| **bash** | `/dev/tcp`, `/dev/udp` (`--enable-net-redirections`) | libc `getaddrinfo`/socket |
| **dropbear** | zlib traffic compression + `DO_HOST_LOOKUP` | zlib port + `getaddrinfo` |
| **wget** | zlib (gzip/deflate) + `--enable-threads=posix` | zlib port + pthreads |
| **NetSurf** | JPEG + WebP image decoding | libjpeg/libwebp ports staged into the NS sysroot |
| **NetSurf** | **JavaScript** (bundled Duktape) | nsgenbind host tool builds the DOM bindings |
| **BusyBox** | upstream `alloc_affinity.c` (CPU-affinity) | real `sched_getaffinity` (stub removed) |
| **curl** | static `libpsl` + `libidn2` + `libunistring` support | static compilation linked to libcurl.a |
| **curl** | `brotli` (`Content-Encoding: br` decode) | decoder-only `libbrotlidec`+`libbrotlicommon` (`tools/ports/build-brotli.sh`) |
| **mbedTLS** | `MBEDTLS_TIMING_C` (timing layer: DTLS retransmit timers, `mbedtls_timing_get_timer`) | `gettimeofday`; timing.c portability `#error` gate taught about b1nix |
| **libc** | `setlocale()`, `localeconv()`, `nl_langinfo()`, `iconv` (UTF-8/UTF-16/UCS4/Latin1/ASCII), `mbrtowc`/`wcrtomb` | native libc implementations |
| **NetSurf** | JPEG-XL (`libjxl`), SVG (`libsvgtiny`), `utf8proc` | CMake/autotools ports staged into the NS sysroot |
| **kernel/libc** | `syslog`/`openlog`/`closelog` deliver to a kernel `/dev/log` sink (→ serial/kernel log); `utmp`/`wtmp` file API; `pam_*` shim over `crypt`/shadow | `/dev/log` char-device write_cb forwards to the kernel log; libc `unistd.c`/`utmp.c`/`pam.c` (all exercised by M29 smoke) |

## M54 closeout

M54 is **done**: every port feature that can be flipped on against a real OS
capability has been (table above), and the rest is either correctly *declined*
or *deferred* behind a real subsystem / large-library port. The deferred items
below are not flag flips — each is genuine work, and several roll into later
browser-platform milestones (M59 LLVM/EGL, modern-HTTP).

### Declined (evaluated, intentionally left off)
- **dropbear `--enable-syslog`** — works (logs flow to `/dev/log`), but moves
  logging off the per-service `/var/log/sshd.log` the M32B lifecycle smoke
  asserts non-empty. Lateral move that breaks a test; stays `--disable-syslog`.
- **dropbear / curl `--harden`** — bundles `-fPIE -pie`, conflicts with the
  fixed `0x2000000` load model.
- **mbedTLS `MBEDTLS_NET_C`** — the custom socket wrapper already works; no
  reason to swap it for mbedTLS's own BSD-socket layer.
- **utmp / pam / lastlog per-port flips** (dropbear `--enable-pam`/`-utmp`/
  `-lastlog`, BusyBox `syslogd`/`last`/`who`) — cosmetic who/last accounting.
  The libc `pam_*`/`utmp`/`wtmp` shims exist; enable per-port only if a real
  need appears.

### Deferred (each a large effort; revisit conditions below)

### System logging (`/dev/log`) and login accounting
- **Foundation: DONE.** `/dev/log` is a kernel char-device sink that forwards
  every datagram to the kernel log (no userspace syslogd needed); libc
  `openlog`/`syslog`/`closelog` write there, and the `utmp`/`wtmp` file API and a
  `pam_*` shim over `crypt`/shadow are real in libc. All verified by the M29
  smoke (`M29-PTHREAD: ok syslog/utmp/pam`, plus the `/dev/log: …` sink line —
  which already captures real port traffic, e.g. BusyBox `passwd`).
- **Remaining (per-port flips), low value:** these now *link*, but were
  evaluated and left off:
  - dropbear `--enable-syslog` — **declined.** It works (sshd logs flow to
    `/dev/log`), but it moves logging *off* the per-service `/var/log/sshd.log`
    that the service script writes and the M32B lifecycle smoke asserts
    non-empty. dropbear already logs to a file; routing it to syslog is a
    lateral move that breaks that, so it stays `--disable-syslog`.
  - dropbear `--enable-harden` — **declined.** Bundles `-fPIE -pie`, which
    conflicts with the fixed `0x2000000` load model.
  - dropbear `--enable-pam`/`-utmp`/`-lastlog` + BusyBox `syslogd`/`last`/`who`
    — cosmetic (who/last accounting); need `/var/run/utmp`, `/var/log/wtmp`
    plumbing. Enable per-port only if a real need appears.

### PAM (pluggable authentication)
- **Unlocks:** dropbear `--enable-pam`; `login`/`su`/`passwd` PAM stacks.
- **Work:** a minimal `libpam` + `/etc/pam.d` policy engine, or a PAM-shim that
  maps to the existing shadow/`crypt` auth.

### Full locale / multibyte
- **Unlocks:** bash `HANDLE_MULTIBYTE`; `nls` (national language support) in
  bash/wget/curl/dropbear; a real `iconv` (NetSurf `libiconv`, `utf8proc`).
- **Work:** wide-char/locale support in libc (`setlocale`, `nl_langinfo`,
  `mbrtowc`/`wcrtomb` beyond the current UTF-8-only path) and a real `iconv`
  with charset tables. Currently libc is UTF-8-wired only.

### Modern HTTP (HTTP/2, HTTP/3) and extra compression
- **Unlocks:** curl/wget `nghttp2` (HTTP/2), `nghttp3` + `ngtcp2` (HTTP/3/QUIC),
  `brotli`, `zstd`; NetSurf JPEG-XL (`libjxl`).
- **Work:** port nghttp2/nghttp3/ngtcp2/brotli/zstd (and libjxl, C++) to the
  b1nix ABI, then enable the corresponding `--with-*` flags.

### Mesa hardware/JIT path and windowing system integration
- **Unlocks:** Mesa `llvm` (LLVMpipe / shader JIT instead of softpipe), `glx`,
  `egl`, `gbm`, `gles1`/`gles2`, `shared-glapi`, shader-cache.
- **Work:** port LLVM to the b1nix ABI (very large) for the JIT path; GLX/EGL/GBM
  only become meaningful once there is an on-device windowing/DRI path beyond the
  current "OSMesa renders into a memory framebuffer" model. LLVM is the long
  pole; the softpipe path already works.

### mbedTLS timing/native-net + misc port hardening
- **Unlocks:** mbedTLS `MBEDTLS_TIMING_C` (timing side-channel countermeasures),
  optionally `MBEDTLS_NET_C` (drop the custom socket wrapper); dropbear/curl
  `--harden` security flags; bash `bash-malloc`.
- **Work:** wire `MBEDTLS_TIMING_C` to a monotonic clock; decide whether to
  replace the custom socket wrapper with `MBEDTLS_NET_C`; validate the harden
  flags do not regress the cross link.
