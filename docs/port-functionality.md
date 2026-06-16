# Third-Party Port Feature Enablement

This document tracks the per-port features that were disabled when each upstream
project was first ported (to get it building at all), which ones have since been
**enabled** as the OS gained the required syscalls/libc APIs, and the remaining
**milestones** for full feature parity. The original "what is disabled and why"
inventory lives in [`../crutches_audit_report.md`](../crutches_audit_report.md)
and [`dehardcode-audit.md`](dehardcode-audit.md).

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

## Deferred (near-term — feasible, blocked on a build detail)

- **curl `--with-libpsl` / `--with-libidn2`.** The libraries are already built
  for the ported wget, but curl's cross `configure` libpsl conftest fails to
  link libpsl.a's transitive deps (`-lidn2 -lunistring`) and reports
  "libpsl libs ... not found". wget already provides PSL/IDN coverage, so this
  is low priority. *Condition:* teach curl's libpsl/libidn2 detection to use the
  full static link chain (or supply pkg-config `.pc` files with `Requires.private`).

## M54 remaining: full feature parity

These are the rest of the disabled features, all under roadmap milestone **M54**.
Each needs a real OS subsystem or a sizeable external library port — genuine
work, not flag flips. (They are *not* separate milestones; this is one tracked
effort.)

### System logging (`/dev/log`) and login accounting
- **Unlocks:** dropbear `--enable-syslog`, `lastlog`, `utmp`/`utmpx`,
  `wtmp`/`wtmpx`; BusyBox `syslogd`/`logger`/`last`/`who`; standard `openlog`/
  `syslog`/`closelog` for every port.
- **Work:** a `/dev/log` datagram sink + in-kernel (or `syslogd`) ring, and the
  `utmp`/`wtmp` file API in libc with a real `/var/run/utmp`, `/var/log/wtmp`.

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
