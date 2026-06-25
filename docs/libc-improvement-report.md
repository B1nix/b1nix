# b1nix libc — assessment vs glibc/musl, and an improvement roadmap

**Status:** read-only analysis (no code changed). Snapshot of `userspace/libc/` +
`userspace/include/` at `B1NIX_VERSION_STR = 0.69.9` (2026-06-24).

b1nix ships a hand-written, freestanding C libc (`libb1nix.a`, ~12 kLOC of C plus
openlibm for `double`/`float` math). It is **C-locale-only**, **UTF-8** for
multibyte, and intentionally musl/glibc-ABI-aliased (the Rust target uses
`os=linux env=musl`). It already supports very large ports — V8/d8, Rust std,
curl+mbedTLS, Mesa, NetSurf, dropbear, GCC/Binutils self-host, and (in progress)
Chromium with clang + libc++. This report inventories what exists, where it is a
stub or approximation, what to adopt (with license guidance), and a prioritized
roadmap.

**TL;DR.** The *surface* is broad and the ABI plumbing is solid; the weak spots
are **numeric correctness** (`strtoul`/`strtoull` cast `strtol`, no overflow/
`ERANGE`; `strtod` not correctly-rounded; `long double` faked), **stdio**
(fully unbuffered, no wide stdio, `perror` prints the literal word "error",
printf lacks `%a`/real `%Lf`/positional args), **i18n** (`iswalpha`/`towlower`
ASCII-only, no real locales/collation/iconv-CJK), **resolver**
(`getaddrinfo` returns a single result, no `/etc/services`), and a handful of
**correctness bugs** (`getprogname()` returns `"wget"`, `realpath` is a
non-resolving `strcpy`, `mktime`==`timegm`, synthetic `d_ino`). Most of these
have drop-in **MIT-licensed musl** implementations.

---

## 1. License guidance (read this first)

This governs *how* every "adopt" item below is allowed to be done.

| Source | License | Can we copy code? | Use it for |
|---|---|---|---|
| **musl** | **MIT** | **Yes** — copy/adapt freely, keep the per-file copyright + the COPYRIGHT notice | The actual *implementations* to port (string ops, `strtod`/`vfprintf`, `getaddrinfo`, `fnmatch`, `glob`, `qsort`, multibyte, the Unicode ctype tables, etc.) |
| **glibc** | **LGPL-2.1+** | **No** (not into a permissive/static freestanding libc — LGPL static-link + "convey the object files" obligations entangle every binary) | *Read for the **API surface, header layout, and macro conventions** only* — never paste code |
| **OpenBSD / FreeBSD libc** | BSD-2/3 | Yes (keep notice) | Secondary source for `strlcpy`/`strlcat`/`arc4random`/`reallocarray` if you prefer the originals |
| **OpenBSD `arc4random`** | ISC/BSD | Yes | `arc4random*` (or just wrap b1nix `getentropy` + ChaCha) |

**Rule of thumb:** when a function needs to be *correct and battle-tested*
(float parsing/printing, collation, resolver, regex, glob), **port the musl
`.c`**. When you only need a header to *parse* (the glibc attribute macros,
`__THROW`, the `<sys/cdefs.h>` shape), **mirror the glibc convention by hand** —
re-typing a one-line `#define` is not copying a copyrightable work, and it keeps
the header license-clean.

Practical mechanics for musl ports:
- Copy the `.c` verbatim where possible; keep musl's internal helper headers
  (`src/internal/*.h`) it pulls, or inline the few macros it needs.
- musl assumes `<sys/cdefs.h>`-free, `<features.h>`-light, weak-alias-heavy
  style; b1nix's `weak`/`alias` usage is compatible (clang + ld.lld).
- Drop musl's `a_*` atomic-helper dependency in favor of b1nix `atomic.c` /
  `__atomic_*` builtins where a file uses `a_cas` etc.
- Add a `THIRD_PARTY/musl-COPYRIGHT` file listing the ported files; that
  satisfies MIT attribution for the whole set.

---

## 2. Inventory — what is present today

Legend: ✅ real · ◑ approximation (works for the common case, deviates at edges)
· ⛔ stub / missing / wrong.

### 2.1 Headers present (`userspace/include/`)

Broad and modern. Present: `string.h stdlib.h unistd.h stdio.h wchar.h wctype.h
ctype.h locale.h pthread.h semaphore.h sched.h signal.h time.h setjmp.h math.h
fenv.h float.h limits.h errno.h(96 E-codes) inttypes.h stdint.h uchar.h iconv.h
langinfo.h nl_types.h libintl.h dlfcn.h link.h elf.h regex.h fnmatch.h getopt.h
glob? netdb.h resolv.h ifaddrs.h arpa/ net/ netinet/ netpacket/ sys/* (socket,
mman, stat, wait, epoll, eventfd, inotify, timerfd, signalfd, ptrace, uio,
resource, auxv, prctl, …) sys/cdefs.h linux/ asm/ machine/`.

**Header gaps:** `threads.h` (C11 threads) ⛔ · `complex.h` ⛔ · `glob.h`/
`wordexp.h`/`ftw.h` ⛔ (only `fnmatch.h` present) · `aio.h` minimal · no
`<sys/cdefs.h>` FORTIFY/`__REDIRECT` machinery.

### 2.2 string.c

✅ `memcpy memset memmove memcmp strlen strnlen strcmp strncmp strcpy stpcpy
strncpy stpncpy strchr strrchr strstr strdup strndup strcat strncat strpbrk
strcasecmp strncasecmp memchr memrchr strtok strtok_r strsep strcasestr strcspn
strspn strchrnul mempcpy strverscmp` · `strerror`/`strerror_r` ◑ · wide ops
`wmemcpy wmemmove wmemset wcslen wcscat wcscpy` compiled here.

- ⛔ **`strlcpy` / `strlcat` missing** (header + impl). Many ports want them.
- ⛔ **`memmem` missing.** (curl/grep-like code uses it.)
- ◑ All ops are **naive 1-byte-per-iteration** — no word-at-a-time/SIMD.
- ◑ `strcoll`→`strcmp`, `strxfrm`→identity copy (C-locale only — correct for C).
- ◑ `strerror` maps ~35 errnos; the other ~60 (e.g. `EOPNOTSUPP`, `EADDRINUSE`)
  return `"Unknown error"`.

### 2.3 stdlib.c

✅ allocator (`malloc/free/calloc/realloc/malloc_usable_size/posix_memalign/
aligned_alloc/memalign`), `qsort` ◑, `bsearch` (inline), env
(`getenv/setenv/putenv/unsetenv/clearenv` + `environ`), `program_invocation_name`
= `"b1nix"`, `atexit`(cap 32), `mkstemp/mkdtemp`, `rand/srand`, `system`,
`strtol/strtoul/strtoll/strtoull/strtod/strtof/strtold`, the `strto*_l` inlines.

- ⛔ **`strtoul`/`strtoll`/`strtoull` just cast `strtol`** → on i686 `strtoull`
  truncates to 32-bit; **no `ERANGE`/saturation anywhere**, errno never set.
  *Real bug* for `uint64`/large-size parsing (JSON, hashes, content-length).
- ◑ **`strtol` has no overflow detection** — silently wraps, no `ERANGE`.
- ◑ **`strtod` not correctly-rounded** (naive decimal accumulation, caps 17
  digits); **`strtof`/`strtold` just cast `strtod`** → no float/long-double
  precision.
- ⛔ **`realpath`→`strcpy(resolved, path)`** — does *not* resolve `..`/`.`/
  symlinks/relative→absolute.
- ⛔ **`getprogname()` returns hard-coded `"wget"`** — wrong for every program.
- ◑ `qsort` is unguarded recursive quicksort → O(n²) + deep-recursion risk.
- ⛔ Missing: `reallocarray`, `arc4random*`, `secure_getenv`,
  `random/srandom/initstate/setstate`, `qsort_r`, `getsubopt`.

### 2.4 stdio.c — **most-approximated area**

✅ `printf/fprintf/sprintf/snprintf/v* dprintf/vdprintf asprintf/vasprintf`,
`fopen/freopen/fdopen/popen/pclose/fclose fread fwrite fgetc fputc ungetc
fseek/ftell/fseeko/ftello fflush feof ferror fileno remove getline
open_memstream tmpfile`, full `scanf` family incl. scansets, `fgets` inline.

- ⛔ **Fully unbuffered** — `setvbuf`/`setbuf`/`setlinebuf` are **no-ops**; every
  `fread`/`fwrite` is one syscall, and `fread` does a **single `read()`** (short
  reads on pipes/sockets — caller-visible truncation). *Big perf + correctness
  item.*
- ⛔ **`perror` prints the literal word `"error"`**, not the errno string. (One
  line to fix; pervasive.)
- ⛔ **No wide stdio** (`fwprintf swprintf vswprintf wprintf fwscanf swscanf
  fwide fputwc fgetwc putwc getwc fputws fgetws ungetwc`). *(`swprintf` etc. are
  declared in `wchar.h` and live in `wchar.c` by transcoding — but the FILE-based
  wide stream set is absent.)*
- printf: ⛔ `%a`/`%A` hex-float · ⛔ real `%Lf`/`%Lg` (the `L` modifier reads a
  `double`) · ⛔ positional `%n$` · ⛔ `%m` · ⛔ locale grouping `'` · ◑ `%e`/`%E`
  only via `%g` internals · ◑ float precision clamped to 9, no inf/nan in `%f`.
- ⛔ **`getdelim` missing** (only `getline`), **`fmemopen` missing**,
  **`open_wmemstream` missing**, `fseek`/`ftell` are `long` (no large-file).

### 2.5 unistd.c (large — also carries time/syslog/sched/fenv/syscall trampoline)

✅ file ops (`read/write/pread/pwrite/readv/writev/lseek/dup/dup2/pipe/fcntl/
ioctl/fsync/ftruncate/mmap/munmap/mprotect/memfd_create/fstatat/utimensat`),
process (`fork/vfork/execve/execvp/execlp/_exit/wait*/waitid`), ids/sessions,
full socket set + `shm_open`, getcwd/chdir/access/readlink/symlink, real
`isatty/tcgetattr/tcsetattr/tcgetpgrp`, `select/poll/ppoll`, `getrandom/
getentropy`, openlog/syslog, `getrlimit/setrlimit`, xattr.

- ◑ `sysconf(_SC_NPROCESSORS_*)` → **hard-coded `1`** (parallel-probing code
  under-provisions). Only ~7 names handled; no `confstr`.
- ⛔ `sethostname` no-op, `chroot` → `EPERM`, `mknod` → `ENOSYS` (so real FIFOs
  via `mkfifo` only if the kernel grows mknod), `mremap` → `ENOMEM`,
  `dl_iterate_phdr` → 0 objects.
- ◑ `nanosleep` granularity **10 ms** (tick-quantized; `rem` always 0).
- ◑ `getlogin` → `"root"`, `ttyname` → always `"/dev/tty"`, `setitimer` only
  `ITIMER_REAL`→alarm, `getitimer` zeros, `wait3/wait4` zero the rusage.
- ◑ fenv: `feclearexcept/feraiseexcept/fetestexcept/fe[gs]etround` are no-ops
  (fixed FPU mode); but `fegetenv/fesetenv/feholdexcept/feupdateenv` are **real**
  x87/MXCSR asm.

### 2.6 posix_compat.c (FFI/ports glue)

✅ `pipe2 accept4 fdatasync mkfifo linkat(AT_FDCWD) pwrite preadv pwritev
sendfile`, **full `posix_spawn` family**, `pthread_atfork` registry, de-inlined
`getpid/getppid/chmod/rename`.

- ⛔ **`sigwait`/`sigwaitinfo` → `ENOSYS`** (thread-pool signal designs fail).
- ⛔ `splice` → `ENOSYS`; `linkat`/`fchmodat` only `AT_FDCWD`; `futimes` no-op.

### 2.7 dirent.c

✅ `opendir/fdopendir/readdir/closedir/dirfd/rewinddir/scandir/alphasort`.

- ⛔ **`d_ino` is synthetic** (per-stream monotonic counter) — breaks `d_ino`↔
  `stat().st_ino` correlation.
- ⛔ Missing `readdir_r seekdir/telldir versionsort getdirentries readdir64`;
  `d_name` capped 63.

### 2.8 netdb.c / resolver

✅ `inet_pton/ntop(v4+v6) inet_aton/addr/ntoa gethostbyname getaddrinfo
freeaddrinfo getnameinfo getservbyname/port gai_strerror res_init if_nametoindex`.

- ⛔ `gethostbyaddr` (reverse), `getprotobyname/number`, `*_r` variants — missing.
- ◑ `getaddrinfo` returns a **single** addrinfo, **numeric port only** (no
  `/etc/services`), no `ai_canonname`, no AAAA path in the common case.
- ◑ `getservby*` = ~6 hard-coded ports; `getnameinfo` numeric-only;
  `if_nametoindex` returns 1 for any non-empty name; no `res_query/res_search/
  dn_expand`.

### 2.9 wide-char / ctype / locale / iconv

✅ `wchar.c` is broad: `mbrtowc/wcrtomb/mbsrtowcs/wcsrtombs/mbsnrtowcs/wcsnrtombs
mbstowcs/wcstombs btowc/wctob wcs* wcwidth wcsftime swprintf/vswprintf` + wide
numeric conversions. `uchar.c` (C11 `mbrtoc16/32`, `c16/32rtomb`) is **fully
correct** UTF-8↔UTF-16. `wctype.c` has the `isw*`/`tow*` + `wctype/wctrans`.
ctype provides the newlib `_ctype_[257]` rune table that libstdc++ wants.

- ⛔ **`iswalpha`/`iswupper`/`towlower`/… are ASCII-only** (`iswalpha`→`isalpha`)
  — **the single biggest i18n correctness gap**: every non-ASCII Unicode letter
  is mis-classified / not case-folded. `_ctype_[0x80..0xFF]` are all zero.
- ◑ wide `*scanf` (`swscanf/fwscanf/wscanf`) → **always `EOF`** (honest no-op).
- ◑ `mbstate_t` not carried across calls (`mbsrtowcs` treats partial tail as a
  hard error) — fine for whole-buffer callers, wrong for true streaming.
- ◑ locale: only `C`/`POSIX`/`*.UTF-8` accepted (others → NULL);
  `newlocale/uselocale` return a singleton; `localeconv`/`nl_langinfo` are
  fixed C/English. No message catalogs (`libintl`/`gettext` are stubs).
- ◑ iconv: 6 charsets (UTF-8/16/32, Latin-1, ASCII, INTERNAL) — **no CJK
  (Shift-JIS/EUC/GB/Big5), no other ISO-8859-x, no //TRANSLIT//IGNORE**.

### 2.10 pthread.c / threading / atomic.c

✅ create/join/detach/exit/self/kill, mutex (NORMAL+RECURSIVE, +timedlock),
cond (+timedwait), rwlock, barrier, TLS keys, once, `attr` (stacksize +
detachstate), deferred cancel — **all real futex-backed**.

- ⛔ **C11 `<threads.h>` absent** (`thrd_*`/`mtx_*`/`cnd_*`/`tss_*`/`call_once`).
- ⛔ `pthread_spin_*` missing; no `pthread_setaffinity_np`; `setname_np` no-op;
  no per-thread signal mask (`pthread_sigmask`→`sigprocmask`).
- ◑ **rwlock is a single mutex** → readers are serialized (no read parallelism);
  no timed rd/wr variants.
- ◑ **`pthread_self()` ≠ the handle from `pthread_create()`** (kernel TID vs
  heap-state pointer, disambiguated by value range) → `pthread_equal(self, h)`
  for the *same* thread is unreliable. Latent POSIX violation.
- ◑ `condattr_setclock` no-op (always CLOCK_REALTIME); TLS get/set is a global
  lock + linear TID scan.

### 2.11 math / float

✅ Full C99 `double`/`float` set via **openlibm** (real); classification macros
(`isnan/isinf/signbit/fpclassify…`) correct as `__builtin_*`; constants/specials
complete; `setjmp/longjmp/sigsetjmp/siglongjmp` real (asm).

- ◑ **`long double` is the native 80-bit type, but `powl` down-casts to
  `double pow`** (precision-lossy) and **every other `*l` is a bare prototype**
  with no dedicated impl — calling e.g. `sinl` at full precision would resolve to
  the double routine or fail to link. `float.h` *advertises* `LDBL_MANT_DIG 64`,
  so this is a precision lie for code that trusts long double.
- ⛔ `<complex.h>` absent. No FP-exception reporting (`math_errhandling =
  MATH_ERRNO` only, no `MATH_ERREXCEPT`).

### 2.12 sys/cdefs.h — glibc convention mirror

Defines `__THROW`/`__THROWNL` (→ `noexcept(true)` in C++ — **load-bearing** for
Chromium's `operator new(...) __THROW`), `__BEGIN/END_DECLS`, `__nonnull`,
`__wur`, `__attribute_malloc__/pure__/const__/used__/noinline__`, `__flexarr`,
`__P`, careful `__restrict` handling. **All attribute macros are empty no-ops**
(parse-only). **Missing:** `__glibc_likely/__glibc_unlikely`,
`__attribute_alloc_size__`, `__attribute_format_arg__`,
`__attribute_warn_unused_result__` (with teeth), `__REDIRECT`, FORTIFY
machinery, `__GLIBC_PREREQ`, `_Noreturn`.

---

## 3. Gap list, prioritized by how often real ports hit it

**Tier A — hit by almost every nontrivial port / silently wrong:**
1. `strtoul`/`strtoull`/`strtoll` cast `strtol`; no overflow/`ERANGE` (numeric
   parsing of large/unsigned values).
2. `perror` prints "error" not the errno string.
3. stdio unbuffered + `fread` single-`read()` short reads (perf + correctness).
4. `iswalpha`/`towlower`/… ASCII-only (i18n classification/case-fold).
5. `strtod` not correctly-rounded; `strtof`/`strtold` = double precision.
6. `realpath` non-resolving `strcpy`.
7. `getprogname()` = "wget".
8. `strlcpy`/`strlcat`/`memmem` missing.

**Tier B — common in larger ports / std libs:**
9. No wide stdio family; printf no `%a`/real `%Lf`/positional args.
10. `getaddrinfo` single-result + no `/etc/services`; `gethostbyaddr`/`getproto*`
    missing.
11. C11 `<threads.h>`, `pthread_spin_*`, real rwlock reader-parallelism,
    `pthread_setaffinity_np`.
12. `sysconf(_SC_NPROCESSORS_*)`=1 (parallelism probes).
13. `reallocarray`, `arc4random*`, `secure_getenv`, `random/srandom`, `qsort_r`.
14. `getdelim`/`fmemopen`; `setvbuf` real buffering.
15. `mktime`==`timegm` (no local-TZ apply); synthetic `d_ino`; `sigwait`=ENOSYS.

**Tier C — niche / large efforts:**
16. Real `long double` math (`*l` family) + `<complex.h>`.
17. Real locales / collation / message catalogs / iconv CJK.
18. `qsort` introsort (O(n²) hardening); SIMD `mem*`/`str*`.
19. POSIX timers (`timer_create`/`setitimer` full/`clock_getres`), real-time
    signals (`sigqueue`/`sigtimedwait`).
20. `glob`/`wordexp`/`ftw`, `<sys/cdefs.h>` FORTIFY + `__glibc_(un)likely`.

---

## 4. Correctness / approximation notes — which actually matter

| Item | Severity for current ports | Why |
|---|---|---|
| `strtoull` casts `strtol`, no `ERANGE` | **High** | V8/Chromium/Rust parse `uint64` everywhere; on i686 this truncates |
| `perror` → "error" | **High (trivial fix)** | Wrong diagnostics across all ports |
| `iswalpha`/`towlower` ASCII-only | **High for i18n** | Browser/editor non-Latin text mis-cased/mis-classified |
| stdio unbuffered / short `fread` | **High** | Perf (every byte = syscall) + truncated reads on pipes |
| `strtod` rounding / `strtold`=double | **Medium** | JS number parsing, V8 already needs only `powl` lossy; last-bit diffs |
| `realpath` = strcpy | **Medium** | Anything resolving symlinks/`..` (shells, build tools) |
| `getprogname()`="wget" | **Medium (trivial)** | `argv[0]`-based diagnostics wrong |
| `pthread_self()`≠create handle | **Medium** | `pthread_equal` mis-compares; rare but real |
| rwlock serializes readers | **Low/Med** | Correct, just no parallel read throughput |
| `getaddrinfo` single result / no services | **Medium** | Multi-A round-robin, named services fail |
| synthetic `d_ino` | **Low/Med** | Breaks `find`-style inode dedup, hardlink detection |
| `mktime`==`timegm` | **Low** | Only wrong off-UTC; b1nix is effectively UTC |
| `long double` faked | **Low today** | Only V8 duration-format used it; openlibm has no 80-bit |
| no locales / catalogs | **Low** | b1nix is deliberately C-locale; ports run in C |

Everything except `perror` and synthetic `d_ino` is an **honest** ENOSYS/no-op
documented in-source — no fake successes.

---

## 5. What to adopt — concrete musl ports + glibc conventions to mirror

### 5.1 Port these musl `.c` files (MIT — copy/adapt, keep notice)

| Need | musl source file(s) |
|---|---|
| `strlcpy`/`strlcat` | `src/string/strlcpy.c`, `strlcat.c` |
| `memmem` | `src/string/memmem.c` (two-way) |
| word-at-a-time `mem*`/`str*` | `src/string/{memcpy,memset,memmove,memcmp,strlen,strchr,strcmp}.c` |
| correctly-rounded `strtod/strtof/strtold` + `strtoul/ull` overflow | `src/stdlib/{strtod.c,strtol.c}` + `src/internal/{floatscan.c,intscan.c,shgetc.c}` (this one port fixes Tier-A #1 and #5 together) |
| robust printf incl. `%a`, real `%Lf`, positional, wide | `src/stdio/vfprintf.c` (+`vfwprintf.c` for wide) — biggest single quality jump for stdio |
| buffered stdio + `getdelim`/`fmemopen` | `src/stdio/*` (the `FILE` buffer model; `getdelim.c`, `fmemopen.c`, `open_wmemstream.c`) — larger, optional |
| `qsort` (introsort/smoothsort) | `src/stdlib/qsort.c` |
| `reallocarray` | `src/malloc/reallocarray.c` |
| `random/srandom/initstate/setstate` | `src/prng/random.c` |
| Unicode `iswalpha`/`towlower`/… tables | `src/ctype/*` + `src/locale/` (musl's compact Unicode tables) — fixes Tier-A #4 |
| `getaddrinfo` multi-result + `/etc/services` + `gethostbyaddr` | `src/network/{getaddrinfo,lookup_name,lookup_serv,gethostbyaddr,getservby*,getproto*}.c` (adapt the resolver back end to `SYS_NET_DNS`) |
| `glob`/`fnmatch`/`wordexp`/`regex` | `src/regex/{glob,fnmatch,wordexp}.c` (already have fnmatch/regex — musl's are higher quality) |
| `mbsrtowcs` streaming `mbstate_t` | `src/multibyte/*` |
| C11 `threads.h` | `src/thread/{thrd_create,mtx_*,cnd_*,tss_*,call_once}.c` (thin over existing pthread) |
| `complex.h` math | `src/complex/*` |
| `strsignal` completeness, `strerror` full table | `src/string/strsignal.c`, `src/errno/strerror.c` style |

### 5.2 Mirror these glibc conventions by hand (LGPL — do NOT copy; re-type)

In `<sys/cdefs.h>` (give the no-op macros real teeth so the optimizer/diagnostics
help, and add the missing ones):

```c
#if defined(__GNUC__) || defined(__clang__)
# define __glibc_likely(c)   __builtin_expect((c), 1)
# define __glibc_unlikely(c) __builtin_expect((c), 0)
# undef  __nonnull
# define __nonnull(p)        __attribute__((__nonnull__ p))
# undef  __wur
# define __wur               __attribute__((__warn_unused_result__))
# undef  __attribute_malloc__
# define __attribute_malloc__ __attribute__((__malloc__))
# define __attribute_alloc_size__(p) __attribute__((__alloc_size__ p))
# define __attribute_format_arg__(p) __attribute__((__format_arg__(p)))
#endif
```

(glibc's reference shape: the GNU `misc/sys/cdefs.h` header — read for the
*structure* only.) Also mirror the glibc **ctype rune-table model**
(`__ctype_b_loc`/`__ctype_tolower_loc`/`__ctype_toupper_loc` accessors) if any
glibc-headered code bypasses b1nix headers, and the glibc **header layout**
conventions (`__BEGIN_NAMESPACE_STD`, the `std::` re-export trick already used in
`<wchar.h>`).

### 5.3 Clean-room / wrap (no upstream needed)

- `getprogname()` → return `program_invocation_short_name` (already set from
  `argv[0]` via crt0). One-line fix.
- `perror` → `fprintf(stderr, "%s: %s\n", s, strerror(errno))`. One-line fix.
- `realpath` → walk components against the VFS (`lstat`/`readlink` loop) — small,
  or port musl `src/misc/realpath.c`.
- `arc4random*` → wrap b1nix `getentropy` + a ChaCha20 stream (or port OpenBSD's).
- `sysconf(_SC_NPROCESSORS_*)` → read the CPU count from `/proc` / sysfs (the
  kernel already exposes SMP CPU count).
- `pthread_spin_*` → trivial atomic test-and-set over existing `atomic.c`.

---

## 6. Prioritized roadmap (effort + what it unblocks)

### Quick wins (≤ ~1 hr each, mostly 1–20 lines)
| # | Fix | Effort | Unblocks |
|---|---|---|---|
| Q1 | `perror` → real `strerror(errno)` | XS | Correct diagnostics everywhere |
| Q2 | `getprogname()` → `program_invocation_short_name` | XS | `argv[0]` diagnostics, BSD-style logging |
| Q3 | Complete `strerror`/`strsignal` tables (all 96 errnos) | S | curl/ssh/Rust error messages |
| Q4 | Add `strlcpy`/`strlcat`/`memmem` (musl) | S | OpenBSD-ish ports, grep/curl |
| Q5 | `reallocarray`, `secure_getenv`, `random/srandom` | S | Rust/curl/coreutils-style ports |
| Q6 | `pthread_spin_*` (atomic TAS) | S | libs that hard-require spinlocks |
| Q7 | `realpath` real resolution (VFS walk or musl) | S–M | shells, make, build tools |
| Q8 | `sysconf(_SC_NPROCESSORS_*)` from kernel CPU count | S | thread-pool sizing (V8, rayon) |

### Medium (½–2 days each)
| # | Fix | Effort | Unblocks |
|---|---|---|---|
| M1 | Port musl `intscan`/`strtol*` → fix `strtoul/ull/ll` overflow + `ERANGE` | M | **Tier-A #1**: correct large/unsigned parsing (V8/Rust/JSON) |
| M2 | Port musl `floatscan`/`strtod` → correctly-rounded + real `strtof/strtold` | M | JS/JSON number fidelity, scientific ports |
| M3 | Port musl `vfprintf` → `%a`, real `%Lf`, positional, `%m`, grouping | M | precise float fmt, i18n format strings, gettext-style |
| M4 | Buffered stdio + `fread` fill-loop + real `setvbuf` + `getdelim`/`fmemopen` | M–L | perf (syscalls→buffer), pipe/socket correctness |
| M5 | Unicode ctype tables → real `iswalpha`/`towlower`/… (musl) | M | **Tier-A #4**: i18n text in browser/editor |
| M6 | `getaddrinfo` multi-result + `/etc/services` + `gethostbyaddr`/`getproto*` | M | real networking (round-robin, named services) |
| M7 | C11 `<threads.h>` shim over pthread | S–M | newer C codebases (some C11 libs) |
| M8 | Fix `pthread_self()`/`pthread_equal` identity; real rwlock readers | M | correct threading semantics, read throughput |
| M9 | Wide stdio family (`fwprintf`/`fputwc`/…, musl `vfwprintf`) | M | wide-locale I/O |

### Larger (multi-day, only if a port demands it)
| # | Fix | Effort | Unblocks |
|---|---|---|---|
| L1 | Real `long double` `*l` math (port openlibm-style 80-bit, or musl) + `<complex.h>` | L | numeric/scientific ports, full C99 math |
| L2 | Real locale framework + collation + message catalogs (gettext) | XL | true i18n apps (probably YAGNI for now) |
| L3 | iconv CJK + ISO-8859-x + //TRANSLIT (musl charmaps) | L | legacy-encoding text |
| L4 | SIMD/word-at-a-time `mem*`/`str*` | M–L | throughput on big movers (Mesa/V8/Chromium) |
| L5 | POSIX timers (`timer_create`/full `setitimer`/`clock_getres`) + RT signals (`sigqueue`/`sigtimedwait`) | L | event-loop/timer-heavy ports, real-time apps |
| L6 | `glob`/`wordexp`/`ftw` (musl) | M | shell/build-tool completeness |

### Suggested sequencing
1. **Quick wins Q1–Q8** in one pass — high signal-to-effort, all low-risk.
2. **M1 + M2 together** (single musl `intscan`+`floatscan`+`shgetc` port) — kills
   the worst numeric-correctness gaps for V8/Rust/Chromium.
3. **M3 + M4** (musl `vfprintf` + buffered FILE) — the stdio quality leap.
4. **M5 + M6** when i18n/networking ports demand them.
5. Defer L-tier until a specific port needs it; **L2 (locales) is likely YAGNI**
   given the deliberate C-locale design.

---

## 7. References

- musl source tree (MIT — the code to borrow):
  <https://git.musl-libc.org/cgit/musl/tree/src> ·
  string: <https://git.musl-libc.org/cgit/musl/tree/src/string> ·
  COPYRIGHT: <https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT>
- musl reference manual: <https://www.musl-libc.org/doc/1.1.24/manual.html>
- glibc `<sys/cdefs.h>` (LGPL — read for conventions only, do not copy):
  <https://codebrowser.dev/glibc/glibc/misc/sys/cdefs.h.html>
- Verified against b1nix sources under `userspace/libc/` and `userspace/include/`
  at v0.69.9 (this report). Recent clang/libc++-port gap history (xlocale `_l`,
  rune table, `mbsnrtowcs`, `__THROW`/noexcept) is captured in the project memory
  note `project_clang_libcxx_port`.
