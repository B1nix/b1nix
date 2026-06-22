# Chromium → b1nix port plan (M60–M63)

Status: **scaffolding only — gated on disk.** A full Chromium checkout is ~50 GB
and the build tree is another ~50–100 GB. This is the binding blocker on the
current dev box (43 GB free). Everything below is real and runnable, but the
fetch/build must happen on a host with ≥150 GB free (or a dedicated volume).

This mirrors the V8 port (`tools/patches/v8/PORT-PLAN.md`), which already did the
hardest shared piece for us — see "Already done" below.

## Already done (free, via the V8 port)

The V8 port patched the **shared Chromium `//build` module**, which full
Chromium uses unchanged:

- `//build/config/BUILDCONFIG.gn` — `target_os == "b1nix"` dispatch to the b1nix
  default toolchain.
- `//build/toolchain/b1nix/` — a real GN toolchain wrapping the
  `x86_64-b1nix-g++` cross compiler.

`tools/patches/v8/apply.sh` applies these to any checkout that has `//build`
(V8 *and* Chromium). So M61's "add b1nix to GN `build/config` + integrate the
cross-toolchain" is substantially in place; the remaining M61 work is the
Chromium-only subsystems (`base/`, `//net`, `sandbox/` stubs, Ozone) that don't
exist in the V8 tree.

## M60 — Ozone headless backend

Ozone backends live in `ui/ozone/platform/`. Plan:

1. Start from the upstream `headless` platform (`ui/ozone/platform/headless`) —
   it has no display-server dependency, so it builds first and gives a bitmap
   target for `content_shell --headless`. Add `b1nix` to the ozone platform
   list in `ui/ozone/ozone.gni` (grep-guarded patch).
2. Once headless renders, add a displayd/Wayland-shaped backend reusing the
   existing libwayland-client port (M49 compositor is the server side).

Verify: `ozone_unittests` for the headless platform, then a `content_shell`
bitmap (M62).

## M61 — Chromium build target

After `gclient sync` (see `tools/sync-chromium.sh`) and re-applying the V8
`//build` patches:

1. `base/` — chase `OS_LINUX`/`__linux__` sites the same way the V8 port chased
   them (`build_config.h` already gets a b1nix branch via the shared patch;
   extend per compile error).
2. `//net` — b1nix has BSD sockets + the M56 epoll/eventfd primitives; expect
   the bulk to be `getifaddrs`/netlink stubs.
3. `sandbox/` — **stub only** (real sandbox is M63). `--no-sandbox` is the
   build-and-run config.
4. Build order, smallest-first: `gn gen out/b1nix` → `ninja -C out/b1nix base`
   → `net` → `mojo` → `content_shell`.

## M62 — content_shell

Target: `content_shell --headless --no-sandbox --screenshot` renders a page to
a PNG and the pixels verify. This is the "Chromium runs" gate. Reuses V8 (M58,
already ported) as the JS engine and OSMesa/EGL (M59) for the GPU path.

## M63 — Sandbox (deferred)

seccomp-bpf + user/PID/net namespaces + setuid sandbox. b1nix has none of
these. Real process isolation only — `--no-sandbox` covers M62. Implement only
if isolation is actually required; otherwise stays a `sandbox/` build stub.

## Progress log

- ✅ Tree fetched + `gclient sync` clean (`build/toolchain_build/chromium/src`, ~30 GB).
- ✅ `tools/patches/chromium/apply.sh` — b1nix `//build` patches apply cleanly to
  Chromium's pinned `//build` (anchors matched → the V8 port's target/toolchain
  work transfers to full Chromium). Patches 1,2,7,8,14,16 + **C1 (is_linux alias)**
  + **C2 (minimal gn_all)**.
- ✅ **Key decision — `is_linux` alias (Patch C1).** At the GN level b1nix now
  counts as `is_linux`, inheriting Chromium's desktop-linux GN logic instead of
  patching thousands of `is_linux` sites (the inverse of V8's own-OS approach).
  b1nix is already `is_posix`.
- ✅ `gn gen` accepts `target_os=b1nix`, clears toolchain/ozone/rust/clang/modules
  config. args in `tools/sync-chromium.sh`.
- ✅ **`gn gen out/b1nix` now PASSES cleanly** (37092 targets, 0 errors). The
  chrome/browser/ui blocker was NOT a content_shell edge — content_shell has
  ZERO chrome deps (`gn path` / `gn desc ... deps --all | grep //chrome` both
  empty). `gn gen` simply evaluates *every* BUILD.gn in the repo (defining, not
  building, all targets), and chrome/browser/ui/BUILD.gn failed its own
  `allow_circular_includes_from` validation because `enable_webui_ntp` was off
  for b1nix. Root cause: `enable_webui_ntp` is gated on the LITERAL
  `target_os == "linux"` (not `is_linux`), so b1nix turned it off and the NTP
  circular-include labels (added unconditionally) had no matching deps (which
  sit under `if (enable_webui_ntp)`). Fixed in Patch C6.
- ✅ Patches now in `apply.sh`: 1,2,7,8,14,16 + C1 (is_linux alias) +
  C2/C3 (root-group scoping) + **C4** (drop GTK linux_ui_factory) +
  **C5** (drop chrome_crashpad_handler) + **C6** (enable_webui_ntp for b1nix —
  THE gn-gen unblocker) + **C7** (ffmpeg os_config → linux x64 asm config).
- ✅ **`is_clang=false` for b1nix (Patch C8) — the compile unblocker.** The
  first b1nix-target objects failed because the build emitted clang-only flags
  (`-Xclang`, `-mllvm`, `--target=`, `-Wgnu`, `-fcolor-diagnostics`, ...) that
  the cross GCC rejects. Cause: `is_clang`'s declare_args default is
  `current_os != "linux" || ...` → TRUE for b1nix. The b1nix toolchain's own
  `toolchain_args { is_clang=false }` is IGNORED because b1nix is the *default*
  toolchain (gn ignores toolchain_args for the default tc). Fixed in
  BUILDCONFIG.gn so b1nix's global default is_clang is false; host clang_x64
  (non-default tc) keeps clang. b1nix objects now compile with `-std=gnu++23
  -fno-exceptions -fno-rtti` (real GCC flags).

- ✅ C9/C10 pthread/dl/rt link model (no -pthread, empty default_libs).

## Current blocker (resume here)

`gn gen` GREEN. In the **`ninja content_shell` compile chase**. Major decisions
locked in and verified by compiling sample objects:

- **C++ stdlib = libstdc++, not libc++ (args.gn `use_custom_libcxx=false`).**
  Chromium's bundled libc++ requires GCC 15+; the b1nix cross GCC is 13.2 →
  `#warning "Libc++ only supports GCC 15 and later"` (a `-Werror=cpp` error).
  b1nix already ships libstdc++ (M55), so use it (matches the b1nix toolchain's
  documented design). Now compiles with `-std=gnu++23` against libstdc++.
- **V8 sandbox OFF (args.gn `v8_enable_sandbox=false`).** With libstdc++,
  `v8/BUILD.gn` asserts `!v8_enable_sandbox || use_safe_libcxx` (the sandbox
  needs libc++ hardening). Sandbox is M63-deferred anyway, and content_shell
  M62 is headless. Turned off.
- **`__linux__` defined for the b1nix target (Patch C11).** The high-leverage
  preprocessor twin of the is_linux GN alias. The b1nix cross GCC predefines
  `__b1nix__`/`__unix__` but NOT `__linux__`, so third_party `#if defined
  (__linux__)` OS checks (perfetto, abseil, skia, ...) fell through to
  "unknown OS". Defining `__linux__` makes them pick the linux path.
  Conflict-free (b1nix sysroot headers don't key on `__linux__`). Verified:
  `obj/third_party/perfetto/.../base64.o` now compiles.

These args.gn keys are persisted in `tools/sync-chromium.sh`. The compile chase
continues — resume with:

```sh
ninja -C build/toolchain_build/chromium/src/out/b1nix -j6 -k0 content_shell
```

Read the first `FAILED:` block's compiler diagnostic (not the command line) in
`build/toolchain_build/gnlogs/ninja_content_shell.log`, root-cause it, add a
grep-guarded perl patch to `apply.sh`, re-`gn gen` if a `.gni` changed, resume.

NOTE: each `gn gen` after a `.gni`/BUILDCONFIG change invalidates the host-tool
(clang_x64) objects, so ninja rebuilds ~370 host steps before reaching b1nix
targets again (~10-20 min). Minimize config churn; batch tree edits.

## Build checkpoint (session 2 end)

`gn gen out/b1nix` GREEN. `ninja -C out/b1nix -j6 -k0 content_shell` compiles
~2270+ b1nix-target objects (base, perfetto incl. trace_processor, protozero, partition_alloc, net, brotli, AND into **skia** GPU/graphite) against the b1nix GCC + libstdc++ sysroot with ZERO failures at the checkpoint. (Reaching skia validates the full fix set deeply.) Total = 71687 steps. The early structural
blockers are all resolved (see patch list + libc gaps below). The next failures
will be the remaining libc function gaps (pthread_getname_np/atfork, wait4,
newlocale/strtod_l) and, later, the content/blink/skia/v8 subsystems.

Resume: `ninja -C build/toolchain_build/chromium/src/out/b1nix -j6 -k0 content_shell`,
read first `^FAILED: obj/` block's compiler diagnostic, root-cause, patch.

## b1nix libc / kernel gaps found by the Chromium build

Unlike the GN/tree patches (which live in `apply.sh` because Chromium re-pulls
them), these are real b1nix OS source changes committed on `chromium-port`. They
ship in the ISO, so each bumps the version.

- **POSIX clock ids** (`userspace/include/time.h` + `kernel/syscall/syscall.c`):
  added `CLOCK_PROCESS_CPUTIME_ID`(2), `CLOCK_THREAD_CPUTIME_ID`(3),
  `CLOCK_MONOTONIC_RAW`(4), `CLOCK_REALTIME_COARSE`(5),
  `CLOCK_MONOTONIC_COARSE`(6), `CLOCK_BOOTTIME`(7) (values match Linux). The
  kernel `SYS_CLOCK_GETTIME` now maps the monotonic family + CPU-time ids to the
  uptime monotonic clock and the coarse-realtime id to the wall clock. (Found by
  perfetto `base/time.h`.)
- **`timegm`** (`userspace/include/time.h`): static-inline UTC `struct tm` ->
  `time_t` (shares mktime's arithmetic, which is already UTC). (perfetto.)

### b1nix libc function gaps still OPEN (next work — found by perfetto/partition_alloc)

`ninja content_shell` reached perfetto + partition_alloc and surfaced these
MISSING libc symbols (the build's `#if defined(__linux__)` paths — now active via
C11 — call full Linux libc). Each needs a real addition to the b1nix libc
(`userspace/`), some with kernel support. These are NOT yet done:

- Trivial header/const additions: `getpagesize()` (return page size 4096 /
  sysconf), `wait4()` (wrap wait/waitpid + rusage), `PR_SET_PDEATHSIG` +
  `prctl` no-op/partial, `SYS_pkey_alloc/free/pkey_mprotect` syscall numbers
  (b1nix has no MPK — return ENOSYS).
- DONE: `pthread_getname_np` (empty name — b1nix stores none), `pthread_atfork`
  (real LIFO-prepare/FIFO-parent-child registry wired into the fork() libc
  wrapper), `sys/cdefs.h` (compat macros), `sys/inotify.h` (honest ENOSYS
  stubs — b1nix has no inotify), `getpagesize`, `sched_*`. 
- STILL OPEN: `newlocale` / `strtod_l` (locale_t — b1nix is C-locale only;
  provide a stub locale_t and route *_l to the non-locale variants), `wait4`.
- GCC-vs-clang `-Werror` strictness (sign-compare, unused-function/variable,
  maybe-uninitialized) in third_party — handled by Patch C12 (demote to
  warnings for the b1nix GCC build; not real bugs). C13 filters those same
  GCC-only flags out of bindgen's libclang invocation (bindgen parses C++ with
  clang, which rejects them).

Strategy: add these to `userspace/` (ships in ISO → version bump), regenerate
the sysroot, resume ninja. Where b1nix genuinely lacks the feature (MPK pkeys,
real scheduling classes, locales) return the POSIX error / C-locale behaviour —
do NOT fake success.

CAVEAT — the toolchain bundles its own copy of the libc headers at
`build/toolchain_build/x86_64-b1nix/cross/x86_64-b1nix/include/` AND a sysroot
copy at `.../x86_64-b1nix/sysroot/usr/include/`. Both were hand-patched so the
CURRENT Chromium build sees the additions, but they are gitignored build
artifacts. On a fresh box these must be regenerated from `userspace/include` by
rebuilding the userspace/sysroot (the cross-toolchain include copy may need a
manual refresh or a toolchain rebuild). The git-tracked source of truth is
`userspace/include/time.h`.

## Compile-chase frontier (session 2 end) — what is fixed vs the next big item

The base/foundational layers now compile against the b1nix GCC+libstdc++ sysroot.
Resolved this session (verified by isolated compile, rc=0): perfetto (incl.
trace_processor), abseil (base/debugging/strings incl. the ELF symbolizer +
raw-syscall paths), partition_alloc, boringssl urandom, skia (reached earlier),
net/base. 15 grep-guarded GN/tree patches (C1-C15) + an extensive b1nix libc
completion (clocks, timegm, getpagesize, sched_*, cdefs.h, inotify stubs,
pthread_getname_np/atfork, ELF DT_*/EI_*/version-types/SHN_, fdatasync, wait4,
locale_t/newlocale/strto*_l, mallinfo, lowercase SYS_* aliases, linux/random.h).

### ✅ RESOLVED — libstdc++ rebuilt with wchar_t (`_GLIBCXX_USE_WCHAR_T 1`)

abseil str_format `arg.cc` needed `std::wcslen`, which libstdc++'s `<cwchar>`
only exposes under `#if _GLIBCXX_USE_WCHAR_T`. The b1nix cross libstdc++ (13.2.0)
had been built with `_GLIBCXX_USE_WCHAR_T` UNDEF because its configure probe
(compile a TU that does `using ::btowc; ... using ::wscanf;` — the FULL wide
set, plus a separate C99 probe for `::wcstold/::wcstoll/::wcstoull` and `::wcstof`
and a `<wctype.h>` `iswblank` probe) failed: the b1nix `<wchar.h>` declared only
the core funcs (wcslen/wcscmp/wcschr/wmem*/mbrtowc/...), so the whole probe — and
therefore the flag — came up `no`.

**Fixed (branch `toolchain-wchar`, v0.65.1):**
1. **`userspace/include/wchar.h`** — added every prototype the probe + `<cwchar>`
   require: `wcsncat/wcsspn/wcscspn/wcspbrk/wcsstr/wcstok/wcsxfrm`,
   `wcstol/wcstoul/wcstoll/wcstoull/wcstod/wcstof/wcstold`, `wcsftime`, and the
   wide-stdio family `fgetwc/getwc/getwchar/fputwc/putwc/putwchar/ungetwc/
   fgetws/fputws/fwide/swprintf/vswprintf/fwprintf/vfwprintf/wprintf/vwprintf/
   swscanf/fwscanf/wscanf`. Now pulls `<stdio.h>` (FILE) + `<stdarg.h>` (va_list)
   and forward-declares `struct tm`.
2. **`userspace/include/wctype.h`** — added `wctrans_t`, `wctrans`, `towctrans`
   (for `<cwctype>`). The `isw*/tow*/iswblank` family was already present.
3. **`userspace/libc/wchar.c` + `wctype.c`** — REAL C-locale implementations:
   wide-string ops are genuine; the numeric `wcsto*` transcode the leading ASCII
   run to a narrow buffer and delegate to the narrow `strto*` (a C-locale number
   is ASCII-only) with the endptr mapped back; wide stdio reads/writes UTF-8
   bytes via narrow fgetc/fputc; the wide printf family transcodes the wide
   format to UTF-8 and delegates to vsnprintf/vfprintf; `wscanf`/`fwscanf`/
   `swscanf` honestly report no conversions (EOF) — not used by the ports, only
   needed so the wchar_t config links. No fakes.
4. **Rebuilt the cross libstdc++ with wchar_t auto-detected** (configure now sees
   the complete sysroot headers). Exact commands (reproducible):
   ```sh
   # stage the completed headers into the cross sysroot + include copies
   cp userspace/include/{wchar.h,wctype.h} build/x86_64/rootfs/include/
   cp userspace/include/{wchar.h,wctype.h} \
      build/toolchain_build/x86_64-b1nix/cross/x86_64-b1nix/include/
   cp userspace/include/{wchar.h,wctype.h} \
      build/toolchain_build/x86_64-b1nix/cross/lib/gcc/x86_64-b1nix/13.2.0/include-fixed/
   cp userspace/include/{wchar.h,wctype.h} \
      build/toolchain_build/x86_64-b1nix/build/build-gcc/gcc/include-fixed/
   # re-detect wchar_t in the existing libstdc++ build tree
   cd build/toolchain_build/x86_64-b1nix/build/build-gcc/x86_64-b1nix/libstdc++-v3
   rm -f config.cache && ./config.status --recheck && ./config.status   # -> _GLIBCXX_USE_WCHAR_T 1
   cd ../..                                                              # build-gcc
   make -j"$(nproc)" all-target-libstdc++-v3 MAKEINFO=true
   make install-target-libstdc++-v3 MAKEINFO=true
   # put the rebuilt b1nix libc (with the new wide funcs) in the sysroot
   cp <worktree>/userspace/build/x86_64/libb1nix.a build/x86_64/rootfs/lib/libb1nix.a
   ```
   On a **fresh box** none of the manual cache-clearing is needed: step 4b of
   `tools/build-toolchain.sh` stages `userspace/include/*` into the sysroot
   *before* libstdc++ configures, so the probe passes and `_GLIBCXX_USE_WCHAR_T`
   is set automatically (documented in build-toolchain.sh step 6).

**Verified (rc=0):** the libstdc++ wchar_t/C99-wchar/wcstof/iswblank configure
probes all compile; a `std::wcslen` + `std::wcstol` + `std::wcsstr` +
`std::iswalpha`/`std::towupper` TU compiles **and links**; and the originally-
failing **`third_party/abseil-cpp/.../str_format_internal/arg.o` compiles for the
b1nix target with rc=0** (`ninja -t commands <target>` → ran the exact compile).

### Other known-remaining
- More `linux/*.h` UAPI headers may be needed as the build advances (provide the
  constants ports actually use, like linux/random.h C-port did).
- More `__linux__`-narrowing for raw-syscall fast paths (cf. C14 abseil mmap) —
  watch for code that does `syscall(SYS_*)` with assumptions about Linux numbers.
- The FINAL LINK needs `libb1nix.a` REBUILT in the sysroot (`cd userspace && make`
  then reinstall) so the new sched_*/wait4/fdatasync/pthread_atfork/locale impls
  are in the archive — only the HEADERS are synced into the build sysroot now.

## How to proceed (fresh box)

```sh
sh tools/sync-chromium.sh      # fetch + apply + gn gen (needs ≥90 GB free)
```

## Session 3 (2026-06-22): wcslen-unblocked build resumes; libstdc++ C99 + libc gaps (v0.65.2)

Branch `chromium-build` (off main, which already has the wcslen toolchain fix +
M65). Resumed `ninja -C out/b1nix -j6 -k0 content_shell`. The build now sails
PAST abseil str_format (wcslen fixed on main) and deep into perfetto/icu/
boringssl. Two fix classes this session, both REAL:

### A. Stale toolchain header copies (re-synced)
`gclient sync`/fresh state had left the cross + sysroot include copies of
`elf.h`, `syscall.h`, `sys/syscall.h` STALE (missing the session-2f
Elf64_Verdef*/SHN_*/lowercase-SYS_* additions that ARE in the repo source).
Re-copied repo `userspace/include/{elf.h,syscall.h,sys/syscall.h}` into both the
cross include dir and the sysroot. (Reminder: those two dirs are gitignored
build artifacts; the repo `userspace/include` is the source of truth.)

### B. libstdc++ C99 detection macros (THE big unblocker, no rebuild needed)
The cross libstdc++ was configured against an empty sysroot, so besides wchar_t
it ALSO recorded that the libc lacked C99 `<math.h>`, `<stdlib.h>`, and the C99
`<stdint.h>` types. That `#if`'d out, across the whole libstdc++:
- `std::signbit/isnan/isinf/isfinite/fpclassify` (`<cmath>`, gated by
  `_GLIBCXX{11,98}_USE_C99_MATH`) — perfetto dynamic_string_writer.
- `std::strtoll/strtoull/atoll/strtof/strtold` (`<cstdlib>`, gated by
  `_GLIBCXX{11,98}_USE_C99_STDLIB`).
- the ENTIRE `<random>` header incl. `std::seed_seq`/engines/distributions
  (gated by `_GLIBCXX_USE_C99_STDINT_TR1`) — perfetto uuid.
b1nix's libc genuinely provides all of these now, so the fix is honest:
`tools/enable-cxx-toolchain.sh` already flipped MATH/STDINT/FENV-TR1 +
`_GLIBCXX11/98_USE_C99_MATH`; this session ADDED `_GLIBCXX{11,98}_USE_C99_STDLIB`
and `_GLIBCXX_USE_C99` to that list, then ran the script (header-only; no GCC
rebuild). Commit 53ed8ec.

### C. b1nix libc gaps (v0.65.2, commit on chromium-build)
New compile-chase batch (perfetto/abseil/ced/sqlite-ICU), all verified rc=0 in
isolation, all REAL (no fakes; unsupported paths return ENOPROTOOPT honestly):
- `<uchar.h>` (NEW) + `uchar.c`: char16_t/char32_t (C-only typedefs) + the C11
  mbrtoc16/c16rtomb/mbrtoc32/c32rtomb conversions. char32_t == b1nix wchar_t so
  the c32 funcs delegate to mbrtowc/wcrtomb; char16_t does real UTF-16 surrogate
  encode/decode (pending surrogate parked in mbstate_t). Needed by ICU (sqlite).
- `string.h`/`string.c`: `memrchr` (ced compact_enc_det).
- `unistd.h`/`unistd.c`: `pwrite` (save/seek/write/restore, mirrors pread).
- `sys/socket.h`: SO_PEERCRED/SO_RCVTIMEO/SO_SNDTIMEO/SO_PASSCRED/SO_RCVLOWAT/
  SO_SNDLOWAT/SO_DOMAIN/SO_PROTOCOL (Linux values) + SOMAXCONN — compile-time
  constants; kernel still returns ENOPROTOOPT for the options it lacks.
- `stdio.h`: FILENAME_MAX/FOPEN_MAX/TMP_MAX.
- apply.sh C12: add `-Wno-error=tautological-compare` (perfetto
  typed_proto_field.h template self-comparison = GCC false positive).

LINK-READINESS: rebuilt `userspace/build/x86_64/libb1nix.a` (`make
B1NIX_ARCH=x86_64`) — now carries memrchr/pwrite/uchar — and copied it over BOTH
`sysroot/lib/libb1nix.a` and `sysroot/lib/libc.a` (the cross-GCC `-lc` resolves
to the latter). So the final link will see the new symbols (and all the
session-2 sched_*/wait4/locale impls, since the whole .a was rebuilt).

Header sync discipline: every changed header copied to repo `userspace/include`
(tracked), the cross include dir, AND the sysroot. `enable-cxx-toolchain.sh`
handles the cross copy + include-fixed; the sysroot copy is manual.

State at session pause: build running single-instance, 0 FAILED, past
icu/boringssl, climbing toward net/mojo/content/blink. NEXT: keep harvesting
`^FAILED:` compiler diagnostics from
`build/toolchain_build/gnlogs/ninja_content_shell.log`, batch-fix (libc gap →
userspace + sync 3 copies + rebuild .a; tree/GN → grep-guarded apply.sh patch;
.gni/BUILD.gn edit → `gn gen` to rebake), verify each in isolation, restart.

### Session 3 cont. — base/ + net/ compile wave (still v0.65.2)

After the libstdc++ C99 + first libc batch, the build reached `base/` and `net/`
and hit a WALL of failures — but mostly ONE root cause cascading:

- **`std::from_range_t` missing (THE base/ unblocker).** Chromium
  `base/containers/circular_deque.h` (transitively included by almost all of
  base/) declares a `circular_deque(std::from_range_t, Range&&)` constructor.
  `std::from_range_t`/`std::from_range` are C++23 (P1206), added in GCC **14**'s
  libstdc++; the b1nix cross compiler is GCC 13.2, which lacks them → a parse
  error (`expected ')' before ','`) that knocked out nearly every base/, net/,
  components/, crypto/, sql/ TU. Fixed by injecting the REAL standard tag type
  into the cross libstdc++ `<bits/ranges_base.h>` (the GCC 14 location), guarded
  off for GCC 14+. Persisted in `enable-cxx-toolchain.sh` (step 5) so a fresh
  box gets it. Verified `base/at_exit.o` rc=0.

- **libc gaps (v0.65.2 batch):** stdint.h PTRDIFF_MAX/MIN + WCHAR/WINT/
  SIG_ATOMIC max/min; endian.h htobe/htole/be*toh/le*toh 16/32/64 (LE host:
  little=identity, big=__builtin_bswap); syscall.h remaining CLONE_* flag bits
  (CHILD_SETTID/PARENT_SETTID/NEW{NS,UTS,IPC,USER,PID,NET}/SYSVSEM/IO/...).

KNOWN-NEXT (seen in the -k0 error harvest, likely real once the cascade clears):
- `clone()` (7-arg glibc wrapper) used by `base/process/launch_posix.cc`
  `ForkWithFlags`. b1nix has no clone-with-stack; honest fix = declare in
  <sched.h> + impl returning ENOSYS (only the namespace/sandbox launch path uses
  it; headless --no-sandbox content_shell shouldn't hit it at runtime).
- `append_range`/`prepend_range` C++23 vector members (same GCC-13-vs-14 gap as
  from_range_t) — may need a similar libstdc++ injection or a Chromium-side
  guard; assess after restart.
- `-Werror=changes-meaning` (base::Value::BlobStorage) and `-Werror=return-type`
  GCC-only diagnostics → likely add to the C12 relaxation set.
- flat_map heterogeneous `find(string_view)` — re-check whether real or cascade.

Restarted the build with all header fixes (header-only → no gn gen). 3 more
commits this session: 53ed8ec, f747178, c259fd2 (sess.3 part 1) + f0f4ed8
(PTRDIFF/endian/CLONE/from_range). Build re-baselining; from_range_t cascade
expected to collapse, leaving the genuine residue above.

### Session 3 cont.2 — base/components/crypto/sql wave (v0.65.2)

The from_range_t fix collapsed the base/ cascade; the remaining failures sorted
into a small set of root causes, all fixed:

- **GCC-only -Werror diagnostics (C12 extended):** -Wno-error=attributes
  (gsl::Owner / cfi-icall attribute-ignored), changes-meaning
  (base::Value::BlobStorage), return-type (switch-covers-all). clang does not
  flag these; not real bugs. Demoted for the b1nix GCC build only.
- **libc gaps (v0.65.2):**
  - <libintl.h> (NEW): no-op GNU gettext (identity). fontconfig's checked-in
    meson-config.h hard-#defines ENABLE_NLS=1 -> #include <libintl.h>.
  - sys/mman.h: MAP_ANON, MS_ASYNC/MS_SYNC/MS_INVALIDATE, msync() (honest no-op:
    b1nix file-mmap is lazy-read through the page cache, no writeback buffer).
- **C17: base/numerics CheckOnFailure::HandleFailure made constexpr** so
  checked_cast() works in a constant expression on GCC 13. PARTIAL — see blocker.
- **C18: content/shell drop testonly rust targets** (rust_test_mojom
  generate_rust + rust_test_service/_ffi) that assert(enable_rust); content_shell
  proper does not need them. Unblocked `gn gen` (was rc=1 assert(enable_rust)).

**OPEN BLOCKER — byte_size.cc / base::ByteSize constexpr on GCC 13.**
`constexpr uint64_t kOneKiB = KiBU(1).InBytes()` (and the 21-ish files that
force constexpr ByteSize/KiBU/MiBU... evaluation) fail: GCC 13 cannot
constant-evaluate the base/numerics CheckedNumeric multiply/ValueOrDie chain
(`ByteSize(kib) * 1024` -> "'kib' is not a constant expression / constexpr call
flows off the end"). Root is deeper than HandleFailure (C17 fixed that and
checked_cast<int64_t>(1) now folds): the CheckedNumeric `state_.value()` /
ValueOrDie path is not constexpr-foldable on GCC 13 (newer GCC/clang handle it).
__builtin_mul_overflow IS constexpr in GCC 13 (verified), and there is no asm
fast-op on x86_64 (BASE_HAS_ASSEMBLER_SAFE_MATH=0), so the remaining gap is in
CheckedNumericState. NEXT: either (a) a targeted base/numerics
CheckedNumericState constexpr patch, or (b) build a GCC-14 cross toolchain
(GCC-14 fixed both from_range_t and this constexpr fold natively). (b) is the
durable fix and would also let us drop the from_range_t/HandleFailure injections.

Commits this sub-session on chromium-build: (libstdc++ C99) 53ed8ec,
(uchar/memrchr/pwrite/socket) f747178, (PORT-PLAN) c259fd2,
(PTRDIFF/endian/CLONE/from_range) f0f4ed8, (libintl/mman/relaxations/C17)
<this batch>, (C18 rust guard) 54df9a6. Build restarted with the full fixset.

### byte_size constexpr — DEEP DIAGNOSIS (don't repeat next session)
Narrowed by isolated compiles (each rc=0 EXCEPT the last):
- `base::checked_cast<int64_t>(param)`            -> folds OK
- `ByteSize(1)`, `ByteSize(1).InBytes()`          -> folds OK
- `ByteSize(1) * 1024` (literal)                  -> folds OK
- `CheckedNumeric<int64_t> c=param; c*=1024; c.ValueOrDie()` -> folds OK
- `checked_cast<uint64_t>(c.ValueOrDie())` w/ param-derived c -> folds OK
- **`ByteSize(kib) * 1024` where kib is a fn PARAMETER -> FAILS** ('kib' is not
  a constant expression / constexpr call flows off the end).
So it's NOT checked_cast, NOT CheckedNumeric mul, NOT ValueOrDie individually —
it is the specific composition through ByteSize's friend `operator*` -> MulImpl
-> AsChecked() (CheckedNumeric from the `bytes_` member) -> ResultType(checked)
ctor, when the seed value is a function parameter (works with a literal). This
is a GCC-13 constexpr template-resolution bug, fixed in GCC 14. The C13 filter +
C17 HandleFailure-constexpr are NOT enough. PROPER FIX = GCC-14 cross toolchain
(also makes from_range_t/HandleFailure injections unnecessary). A Chromium-side
workaround would have to rewrite KiBU/MiBU/.../the ByteSize operator* to avoid
the member-AsChecked path for the constexpr-foldable widening case — invasive
and touches base logic, so prefer the toolchain bump. ~21 non-test files force
this constexpr eval (grep `constexpr.*=.*[KMGTPE]iBU?\(`).

### byte_size constexpr — RESOLVED (C20). Real root: P2564, not CheckedNumeric.
The deep-dive above pointed at CheckedNumeric, but the ACTUAL root was simpler:
`base::ByteSize`/`ByteSizeDelta`'s signed-integer constructors are **consteval**
(to reject out-of-range constants at compile time), while the KiBU()/MiBU()/...
helpers are **constexpr** and call `ByteSize(kib)` with their PARAMETER. On a
C++23 compiler, P2564 (immediate-function escalation) promotes the helper to an
immediate function so the consteval call is fine. **GCC 13 does not implement
P2564**, so it rejected "kib is not a constant expression". (That's why every
isolated piece folded but the composed `ByteSize(param)*N` did not — `ByteSize`'s
signed ctor was consteval, and only the param-seeded call exercised it.)

FIX (C20): relax those two ctors `consteval` -> `constexpr`. The value is still
range-checked by checked_cast — at runtime via CHECK instead of compile time —
so it's correctness-preserving and honest, only deferring the compile-time
out-of-range diagnostic. Verified base/base/byte_size.o rc=0. This unblocks
base/ (the gate to net/mojo/content/blink). Drop C20 once the cross toolchain is
GCC 14+ (implements P2564; would also retire the from_range_t + HandleFailure
injections). M67 (GCC-14 toolchain) tracked in roadmap as the durable path.

PATCH SET now C1-C20 (+ enable-cxx-toolchain.sh libstdc++ C99/wchar/from_range +
the v0.65.2 libc additions: uchar/memrchr/pwrite/socket-consts/stdio-maxes/
PTRDIFF/endian/CLONE/libintl/mman-msync). Build restarted with the FULL set;
base/ now expected to compile through. Next failures (if any) will be net/mojo/
content/blink/skia or the final LINK (libb1nix.a already rebuilt in the sysroot).

## Session 4 (2026-06-22): stale toolchain headers (AGAIN) — re-synced, no new code

Resumed on branch `chromium-m62-content-shell` (off main 0141019, which already
carries C1-C20 + the v0.65.2 libc additions in `userspace/include`). The build
had ~14000 cached objects. First `ninja -j6 -k0 content_shell` pass produced 97
FAILED across perfetto (89), abseil (6), partition_alloc (2). EVERY failure was
the SAME root cause as session-3 §A: the **toolchain's header copies were stale**
relative to the v0.65.2 source headers. Specifically:
- `cross/x86_64-b1nix/include/*` (the `$prefix/$target/include` dir GCC searches
  by default — takes priority over `--sysroot`) and `cross/.../include-fixed/*`
  were OLD copies missing CLOCK_BOOTTIME/timegm (`time.h`), `struct mallinfo`
  (`malloc.h`), `pthread_atfork` (`pthread.h`), lowercase `SYS_write`/
  `SYS_rt_sigprocmask` (`sys/syscall.h`), `Elf64_Versym/Verdef/Verdaux`
  (`elf.h`); and `include-fixed/stdlib.h` still had the OLD `int*` sem_init/
  sem_wait/... decls that now conflict with `<semaphore.h>`'s `sem_t*` ones.
- The sysroot include (`build/x86_64/rootfs/include`, symlinked as the sysroot)
  was ALSO stale (the userspace `install-headers-libs` had not been re-run after
  the header edits landed on main).

FIX (no source change — pure re-sync of gitignored build artifacts):
  1. `make -C userspace B1NIX_ARCH=x86_64 install-headers-libs`
     -> refreshes `build/x86_64/rootfs/include/*` + reinstalls libb1nix.a.
  2. `sh tools/enable-cxx-toolchain.sh x86_64-b1nix`
     -> `copy_if_changed` re-syncs `userspace/include/*` into
       `cross/x86_64-b1nix/include/*` AND the `include-fixed/*.h` that have a
       b1nix counterpart (this is what cleared the sem_init/stdlib.h conflict).
All 97 failures verified rc=0 in isolation after the re-sync (perfetto time.o,
partition_root.o, allocator_shim, abseil raw_logging/vdso_support/sem_waiter).
Killed the contaminated ninja, restarted fresh -- partition_alloc now links,
0 failures through icu/base.

REMINDER for next resume (and a candidate durable fix): BEFORE running ninja,
always run steps 1+2 above so the toolchain headers match `userspace/include`.
Nothing here belongs in apply.sh -- the source headers are already correct on
main; only the cached toolchain copies drift. (A one-liner wrapper that runs
both before `ninja` would prevent this recurring for a 3rd time.)
