# M67 — Rust → b1nix cross-toolchain (RUST-PORT branch)

Status: **KEY MILESTONE MET.** A real Rust std program (Vec / String / HashMap /
iterator / `std::thread` spawn+join) **compiles and links** for
`x86_64-unknown-b1nix` into a clean static b1nix ELF. Runtime QEMU verification
is wired (M67-RUST smoke) but gated on an unrelated in-flight toolchain fix
(see "Blocked on" below) and an idle host.

This branch must **not** be merged into `main` until the port is fully working
(runs in QEMU). Commits live on `rust-port`.

## Approach (what was built)

b1nix is linux-ABI-aliased with a POSIX libc, so Rust std's `unix` PAL is reused
unchanged rather than writing a new `std::sys` backend:

- **Target spec** `build/rust/targets/x86_64-unknown-b1nix.json`
  - `os=linux`, `env=musl`, `llvm-target=x86_64-unknown-linux-gnu` → std picks
    the `unix` PAL and the Linux flavor of every `std::sys` module.
  - `panic-strategy=abort`, static ET_EXEC, `position-independent-executables=false`,
    `relocation-model=static`, `dynamic-linking=false`.
  - `linker=x86_64-b1nix-gcc` (the b1nix cross gcc).
  - `pre-link-args.gnu-cc = ["-m64","-static","-no-pie","-Wl,-Ttext-segment=0x2000000"]`
    so the binary loads at the fixed b1nix userspace base (0x2000000) — its
    `_start` is the b1nix crt0, which calls Rust's C-ABI `main`.

- **libc shims** `userspace/libc/rust_compat.c` (+ headers) — real impls of the
  glibc/musl symbols std links but b1nix lacked: `__errno_location`, `getauxval`
  (walks the auxv past environ's NULL), `bcmp` (weak), `__xpg_strerror_r`,
  `pause`, `pthread_attr_get/setguardsize` (0 — no guard pages), inert
  `_Unwind_*` stubs (panic=abort, never invoked).

- **futex bridge** in `userspace/libc/unistd.c`: `syscall(202, ...)` (the Linux
  futex number std issues directly) is translated to b1nix native
  `SYS_FUTEX(uaddr, op{WAIT/WAKE}, val, timeout_ms)` — masks `FUTEX_PRIVATE_FLAG`,
  maps `FUTEX_WAIT_BITSET`/`WAKE_BITSET`, converts the absolute CLOCK_MONOTONIC
  deadline std passes into a relative ms timeout. This is what makes
  `std::thread`/`Mutex`/`Condvar` work.

- **sysroot staging**: `tools/build-rust-toolchain.sh` copies the freshly built
  `libb1nix.a` (as both `libb1nix.a` and `libc.a`), `crt0.o`, and
  `userspace/include/*` into the cross-gcc sysroot
  (`build/toolchain_build/x86_64-b1nix/sysroot/{lib,include}`), so the gcc
  resolves `-lc`, the crt0 startfile, and headers natively.

## How std builds

`cargo +nightly build --release -Z build-std=std,panic_abort -Z json-target-spec
--target <spec>.json`. All of core/alloc/std/libc/hashbrown/object/gimli/... compile
from rust-src; `hello-b1nix` links. Two nightly-gate notes:
- `-Zjson-target-spec` is now **required** to pass a `.json` target to cargo.
- the old `build-std-features=panic_immediate_abort` is gone — it became the
  real panic strategy `immediate-abort`; we just don't use it (Cargo.toml +
  target both set `panic=abort`).

## Verified (compile + link)

```
rustc 1.98.0-nightly (2026-06-15) / cargo 1.98.0-nightly
$ sh tools/build-rust-toolchain.sh
   ...Compiling std v0.0.0 ... Compiling hello-b1nix ... Finished
$ file build/rust/hello-b1nix/target/x86_64-unknown-b1nix/release/hello-b1nix
ELF 64-bit LSB executable, x86-64, statically linked
$ readelf -h <bin> | grep -E 'Type|Machine|Entry'
  Type: EXEC   Machine: X86-64   Entry: 0x20041f4   (within 0x2000000 base)
$ readelf -d <bin>            # -> "There is no dynamic section" (fully static)
$ nm -u <bin>                 # -> empty (no unresolved symbols)
```
`_start` (0x20041f4) is the b1nix crt0 (pops argc, sets argv/envp/environ, runs
ctors, calls `main`). The crate `main` is the C-ABI entry Rust generates.

## Repro

```sh
# one command (stages sysroot + builds the demo):
sh tools/build-rust-toolchain.sh
# regenerate the committed runtime-test blob after changing the .rs or shims:
sh tools/blobs/build-rust-hello.sh
```
Env it relies on (all under build/, gitignored): build/rust/{rustup,cargo}
(nightly + rust-src), build/rust/targets/*.json, the x86_64-b1nix cross gcc.

## Runtime smoke (wired, gated)

x86_64-only, mirrors the M40 prebuilt-blob pattern:
- `tools/blobs/hello_b1nix.elf` committed; Makefile `initramfs_m67_rust.inc` xxd's it.
- `initramfs.c` registers `/bin/m67-rust` (inside `#ifdef __x86_64__`).
- `programs.c` spawns it, waits, emits `M67-RUST: ok run-std` on exit 0.
- `smoke.sh` asserts the printed `squares=[0, 1, 4, 9, 16] sum=30` + `thread
  returned 42` lines and the ok/done markers.

## Blocked on (NOT a Rust issue)

A full `make ARCH=x86_64 b1nix.test=1 iso` does not build on this branch's base
(a036c3c): the cross **libstdc++** was configured without `wchar_t`, so NetSurf's
`libjxl` dependency fails compiling `<cwctype>` (`wctrans_t`/`towctrans`/`wctrans`
not declared). This is owned by the **`toolchain-wchar`** branch (commit
`051909e` "complete the wide-char family for libstdc++ wchar_t") and needs a
cross-libstdc++ rebuild. The Rust port does not touch this; editing
`userspace/include/*` merely invalidates the (NetSurf) initramfs incs, which is
what surfaces the pre-existing toolchain bug.

## Next steps (precise)

1. **Runtime verify** once `toolchain-wchar` lands (rebase rust-port onto it or
   cherry-pick 051909e + rebuild the cross libstdc++) AND the host is idle
   (load < ~3; the chromium clang build saturates 8 cores → smoke flakes per the
   project's known oversubscription gotcha):
   ```sh
   make ARCH=x86_64 KERNEL_CMDLINE="b1nix.test=1" iso
   sh tests/smoke.sh x86_64        # expect: M67-RUST: ok run-std / done
   ```
   If `_start`/argv handoff misbehaves, check the crt0 stack layout vs Rust's
   C-ABI `main(argc,argv,envp)` (already confirmed matching by disassembly).
2. **Bump the version** to a patch (e.g. 0.64.15 over this base, or fold into
   the milestone minor `0.67.0` once M67 is declared done) only after the QEMU
   run passes — it ships a new initramfs binary + libc symbols.
3. **Chromium wiring** (final M67 step — do NOT edit the Chromium tree's rust
   config while the content_shell build agent owns it; document only):
   - Chromium uses `rust_bindgen` / `rust.gni` with `rustc_libstd` +
     `custom_toolchain`. Point `rust_sysroot_absolute` at the b1nix-built
     std (`build/rust/.../target/x86_64-unknown-b1nix`) or supply
     `--sysroot`-equivalent via `rustc_args`, and set the Rust target to
     `x86_64-unknown-b1nix.json` with the same `-Zbuild-std` flags. Chromium's
     `//build/config/rust.gni` `rust_abi_target` must match this triple.
   - Provide the cross linker (`x86_64-b1nix-gcc`) to Rust via `[target.<triple>]
     linker` in a cargo/config or the gn `rust_*` linker var.
4. **Re-index the knowledge graph** after the branch closes
   (`index_repository(mode="full")` via the codebase-memory MCP).

## Open items / known limits

- `getauxval` returns 0 when AT_PAGESZ is absent (std treats 0 as "use default").
- futex bridge handles WAIT/WAKE (+ _BITSET); other futex ops return ENOSYS
  (std only uses wait/wake). Timeout is ms-granular (b1nix native limit).
- panic=abort only (no unwinding) — intentional; `_Unwind_*` are inert stubs.
- i686/`ARCH=x86` Rust target not attempted (the spec is x86_64-only); the
  smoke is x86_64-guarded.
