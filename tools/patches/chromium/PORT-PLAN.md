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

## How to proceed (fresh box)

```sh
sh tools/sync-chromium.sh      # fetch + apply + gn gen (needs ≥90 GB free)
```
