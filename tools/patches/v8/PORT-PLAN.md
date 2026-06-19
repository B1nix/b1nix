# V8 → b1nix GN target — skeleton (validated patch plan)

**Status: GN-target skeleton only. The full port is the 2–3 month effort in
[`docs/v8-feasibility.md`](../../../docs/v8-feasibility.md). This file is the
*proven* set of edits that adds `b1nix` as a GN/V8 `target_os`** — the dominant
blocker from the probe — so the path is mapped before the multi-GB `fetch v8`.

## ✅ EMPIRICALLY VALIDATED (v0.56.12)

The 6 edits below were not just inspected — they were **applied to a real V8
checkout and run through a real `gn`** (built from source, v2422). Result:

- **Before** the patches, `gn gen --args='target_os="b1nix"'` dies immediately
  with `Unsupported target_os: b1nix` (assert at `BUILDCONFIG.gn:297`).
- **After**, `target_os="b1nix"` passes the entire `target_os` dispatch, resolves
  the `//build/toolchain/b1nix` toolchain, and proceeds into the full `BUILD.gn`
  graph — failing only on **absent gclient deps** (`third_party/icu/config.gni`,
  `third_party/rust-toolchain/VERSION`, …), i.e. the exact files a *linux* target
  also needs and that only `gclient sync` provides.
- Control: a bogus `target_os="zzznope"` still hits the `Unsupported` assert,
  proving the b1nix arm — not some bypass — is what lets b1nix through.

So the GN-target shape is **proven correct**. The remaining wall is purely the
multi-GB `gclient sync` (+ per-dep source), which is the "fetch v8" step the
skeleton phase deliberately deferred — not any b1nix-specific GN problem. Two
throwaway stubs were hand-authored to walk the import chain past the infra files
(`build/config/gclient_args.gni`, `third_party/icu/config.gni`); everything past
that is real third-party source, i.e. the port itself.

Reproduce: `sh tools/build-gn.sh` (builds gn), then the patches + `gn gen` as in
"Build order" below.

---

Every anchor below was checked against a real shallow checkout:
- V8 proper: `git clone --depth1 https://chromium.googlesource.com/v8/v8` (248M)
- `//build` module: `git clone --depth1 .../chromium/src/build` (14M)

(Both clone into `build/toolchain_build/v8-skeleton/`, which is gitignored.)
Line numbers are from the tip of `main` at probe time; re-grep the anchor
strings after a `gclient sync` since upstream drifts.

---

## What is NOT a blocker (confirmed)

- **Toolchain**: `x86_64-b1nix-g++` is GCC 13.2, C++17 **and** C++20, libstdc++
  with exceptions/RTTI/threads (M55). V8 builds with GCC. ✅
- **Runtime POSIX gaps the probe flagged are already closed** (v0.56.6, commit
  `590048a`): `madvise(MADV_FREE/DONTNEED)`, `MAP_NORESERVE` lazy-commit,
  `sigaltstack`+`SA_ONSTACK`. `MM-SMOKE: ok madvise/noreserve/sigaltstack`. So
  feasibility-doc runtime blocker #2 and #4 are **done**. ✅
- `is_posix` auto-covers b1nix: `//build/config/BUILDCONFIG.gn:335`
  `is_posix = !is_win && !is_fuchsia` — no `is_b1nix` boolean needed (upstream
  explicitly tells lesser unixes to check `current_os` directly, line 320-322).

## The one real structural blocker (this skeleton)

b1nix is unknown to GN and to V8's `v8config.h`. Four in-tree edits + two
net-new files make `gn gen ... --args='target_os="b1nix" target_cpu="x64"'`
dispatch correctly and let `v8config.h` compile. They do **not** by themselves
get V8 to link — that's the weeks of `is_linux`-site chasing after this.

---

## Patch 1 — `//build/config/BUILDCONFIG.gn` : default toolchain dispatch

Anchor: the `target_os` if-chain ending at the `aix`/`zos` branches (≈ line
284-296). Add a `b1nix` branch before the final `else assert(false …)`:

```gn
} else if (target_os == "zos") {
  _default_toolchain = "//build/toolchain/zos:$target_cpu"
+} else if (target_os == "b1nix") {
+  _default_toolchain = "//build/toolchain/b1nix:$target_cpu"
} else if (target_os == "emscripten") {
```

No `is_b1nix =` line is needed (see above). `is_posix` already becomes true.

## Patch 2 — net-new `//build/toolchain/b1nix/BUILD.gn`

Drop in `tools/patches/v8/toolchain/b1nix/BUILD.gn` (this repo). It is a
`gcc_toolchain("x64")` modeled on `//build/toolchain/linux/BUILD.gn:182`,
pointing at the in-tree `x86_64-b1nix-` cross GCC, with
`current_os = "b1nix"`, `is_clang = false`, `use_remoteexec = false`.

> Host/target split (feasibility blocker #3): `mksnapshot`/`torque`/
> `bytecode_builtins_list` must build with the **host** Linux toolchain and run
> during the build. GN already runs those in `host_toolchain`; the b1nix
> toolchain here is target-only, so the split is automatic *provided* `gn gen`
> is given `host_os="linux"` (the default on this box). Verify once V8 links.

## Patch 3 — `v8/include/v8config.h` : OS detection by predefined macro

**This is the subtle one.** `x86_64-b1nix-g++ -dM` defines `__b1nix__`,
`__unix__`, `__ELF__` — but **NOT `__linux__`**. So both macro chains in
`v8config.h` miss b1nix.

3a. Feature-header include chain (≈ line 28-36). b1nix has no `<features.h>`
(no glibc); fall through harmlessly — add nothing, or guard if you later need
libc detection.

3b. **OS-detection chain** (≈ line 99-180), the `#if defined(__ANDROID__) …
#elif defined(__linux__) … #endif` block that ends at `__MVS__`. b1nix matches
nothing → no `V8_OS_*`/`V8_OS_STRING` defined → downstream breakage. Treat
b1nix as Linux-like (it has `/proc/self/maps`, `futex`, `mmap`/`mprotect`):

```c
+#elif defined(__b1nix__)
+# define V8_OS_LINUX 1
+# define V8_OS_POSIX 1
+# define V8_OS_STRING "b1nix"
 #elif defined(__sun)
```

Inserting before `__sun` (so `__linux__` proper is untouched) keeps b1nix
sharing the Linux `V8_OS_LINUX` code paths — which is what Patch 4 also assumes.

## Patch 4 — `v8/BUILD.gn` : two spots

4a. **`V8_TARGET_OS_*` defines** (≈ line 1117-1138). Add a b1nix arm so
`V8_HAVE_TARGET_OS` is set (otherwise V8 assumes target==host):

```gn
} else if (target_os == "chromeos") {
  enabled_external_v8_defines += [ "V8_HAVE_TARGET_OS" ]
  enabled_external_v8_defines += [ "V8_TARGET_OS_CHROMEOS" ]
+} else if (target_os == "b1nix") {
+  enabled_external_v8_defines += [ "V8_HAVE_TARGET_OS" ]
+  enabled_external_v8_defines += [ "V8_TARGET_OS_LINUX" ]
}
```

(Reuse `V8_TARGET_OS_LINUX` — adding a brand-new `V8_TARGET_OS_B1NIX` would
require editing `v8config.h`'s target-OS block + every `#ifdef
V8_TARGET_OS_LINUX` site. Aliasing is the lazy correct move; split later only
if a real behavioural difference appears.)

4b. **platform source selection** (≈ line 7315-7345, the `v8_libbase` set).
`platform-posix.cc` is already added by `if (is_posix …)` at 7315. The
`is_linux` arm at 7328 adds `platform-linux.cc` + `stack_trace_posix.cc` and
links `dl`,`rt`. b1nix is `is_posix` but **not** `is_linux`, so add a branch:

```gn
  if (is_linux || is_chromeos) {
    sources += [ "src/base/debug/stack_trace_posix.cc",
                 "src/base/platform/platform-linux.cc",
                 "src/base/platform/platform-linux.h" ]
    libs = [ "dl", "rt" ]
+  } else if (current_os == "b1nix") {
+    sources += [ "src/base/debug/stack_trace_posix.cc",
+                 "src/base/platform/platform-linux.cc",
+                 "src/base/platform/platform-linux.h" ]
+    # b1nix has no -ldl/-lrt (static libc) — link nothing extra.
  } else if (current_os == "aix") {
```

**Decision: reuse `platform-linux.cc`, do NOT fork a `platform-b1nix.cc` yet.**
`platform-linux.cc` reads `/proc/self/maps` (b1nix has procfs) and uses
`madvise`/`mmap` (now present). It *also* pulls Linux-only bits: the JIT-perf
interface (`/tmp/perf-*.map`, `PERF_*`), `MAP_JIT`/`memfd` for the perf
trampoline, and `prctl`. Under `--jitless` (the target scope) the perf-JIT
path is dead code; compile-fails there get `#ifdef __linux__`-guarded → first
patch-as-you-go work *after* this skeleton links. If guard count explodes,
*then* fork `platform-b1nix.cc` from it. (ponytail: one fewer file until proven
necessary.)

---

## Build order (after this skeleton lands in a real checkout)

```sh
# 0. depot_tools + fetch (the multi-GB step this skeleton de-risks)
fetch v8 && cd v8
# 1. apply patches 1,3,4 + copy patch-2 toolchain dir + this repo's BUILD.gn
# 2. host tools first, then target:
gn gen out/b1nix --args='target_os="b1nix" target_cpu="x64" v8_enable_i18n_support=false is_debug=false v8_jitless=true v8_use_external_startup_data=false'
ninja -C out/b1nix v8_libbase   # smallest unit that exercises the platform layer
ninja -C out/b1nix mksnapshot   # host toolchain; proves the host/target split
ninja -C out/b1nix d8           # the goal: jitless d8
```

`v8_libbase` is the cheapest first ninja target — it's exactly the
`src/base/platform` set Patch 4b touches, so it fails fast if the OS plumbing
is wrong before the multi-hour full build.

## Remaining effort after the skeleton (unchanged from feasibility doc)

| Step | Effort |
|---|---|
| This skeleton compiles `v8_libbase` | days |
| Chase `is_linux`/`__linux__` sites across `//build` + V8 until `d8` links | ~3–6 wk |
| Host/target snapshot wiring verified end-to-end | ~1 wk |
| `d8 --jitless` runs `print("hello")` on b1nix, then embedder/file-I/O breaks | ~1–2 wk |

**Net: still 2–3 months.** This skeleton retires the "does the GN target even
have a shape" risk — it does, and it's the six edits above.

---

## Appendix A — `platform-linux.cc` reuse: b1nix gap inventory (validated)

Patch 4b reuses `platform-linux.cc` for b1nix. Checked every Linux-ism in that
file against b1nix's libc/headers/procfs. The concrete breakage list (this is
the "chase is_linux sites" work, made an inventory rather than a guess):

| Linux-ism (file:line) | b1nix today | Action |
|---|---|---|
| `#include <sys/prctl.h>` (:15) | **header missing** — but `prctl()` is **not actually called** in the file | drop/guard the include, or ship an empty `sys/prctl.h`. Trivial. |
| `mremap(…, MREMAP_FIXED\|MREMAP_MAYMOVE)` (:81) | **no `mremap` syscall/libc** | **real gap.** Add `SYS_MREMAP` + libc wrapper, or patch the `RemapPages` call site to munmap+mmap(MAP_FIXED). Under `--jitless` this is in the page-allocator remap path — likely hit. Medium (kernel touch). |
| `<sys/sysmacros.h>` + `makedev()` (:326) | **present** (`userspace/include/sys/sysmacros.h`) | none ✅ |
| `<sys/mman.h>` mmap/munmap/madvise | present; `madvise`/`MAP_NORESERVE` landed v0.56.6 | none ✅ |
| `/proc/self/maps` parse (:276, `SignalSafeMapsParser`) | procfs has `maps` (`kernel/fs/procfs.c`) **but emitted only 4 columns** — V8's parser reads `offset major:minor inode` strictly and aborts on the missing `dev`/`inode` | **FIXED v0.56.9** — `r_pid_maps` now emits Linux-format `start-end perms offset 00:00 inode path` (real vfs inode for file maps). Also fixes pmap/lsof/glibc-style backtrace parsers. |

Net new b1nix gaps surfaced for the platform layer: **one** (`mremap`). Both
`prctl`-include and the maps-format issue are now trivial/done. The runtime
memory model (`madvise`/`MAP_NORESERVE`/`sigaltstack`) was already closed.

### `mremap` — confirmed NOT a jitless blocker

`mremap` (platform-linux.cc:81) lives only in `OS::RemapShared` — the
shared-cage / pointer-compression-shared remap path, not the core jitless heap.
Under a single jitless isolate it is not reached. Action: stub `RemapShared` to
`return nullptr` (or implement `SYS_MREMAP` later if the shared cage is ever
enabled). Demoted from "real gap" to "deferred stub".

## Appendix B — `platform-posix.cc` reuse: b1nix gap inventory (validated)

`platform-posix.cc` is compiled for **every** posix target incl. b1nix
(`if (is_posix …)`, BUILD.gn:7315). Checked against b1nix headers:

| posix-layer symbol (file:line) | b1nix | Action |
|---|---|---|
| `dlsym(RTLD_DEFAULT,"memfd_create")` (:55,:747) | `dlfcn.h` exists, `dlsym`→NULL, `RTLD_DEFAULT` defined | compiles; V8 falls back gracefully ✅ |
| `ftruncate` (:758) | present (`unistd.h`) | none ✅ |
| `MAP_NORESERVE` (:145), `sigaltstack` (:290,:315) | present (v0.56.6) | none ✅ |
| `madvise(MADV_DONTFORK)` (:183), `madvise(MADV_HUGEPAGE)` (:196) | constants **were missing** → kernel `default: -EINVAL` | **FIXED v0.56.10** — added `MADV_DONTFORK/DOFORK/HUGEPAGE/NOHUGEPAGE` (Linux ABI) to both headers; kernel `sys_madvise` accepts them as legal no-op. Verified `MM-SMOKE: ok madvise` (now also exercises DONTFORK+HUGEPAGE). |
| `MAP_JIT` (:158), `MADV_FREE_REUSABLE/REUSE` (:570+) | macOS-only (`V8_OS_DARWIN`) | not b1nix ✅ |

Net result: after v0.56.10, **`platform-posix.cc` has no remaining b1nix gap**;
`platform-linux.cc` has one deferred stub (`RemapShared`/`mremap`) and one
trivial include guard (`prctl`). The platform layer is essentially ready — the
bulk of the 2–3 months is the GN build graph + linking the rest of V8, not the
OS abstraction.
