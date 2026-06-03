# NATIVE-SMOKE / 0x2000000 "fixed-load collision" — RESOLVED

**Status: FIXED** (2026-06-03, branch `x86-32bit-port`). It was never a PMM /
page-table / layout bug — the kernel was embedding a **stale x86_64
`native_smoke` binary** in the 32-bit initramfs. The whole "layout-flaky"
framing below (kept for history) was a red herring.

## Root cause

`native_smoke` is built into the **shared, non-arch-qualified** directory
`userspace/build/bin/`. Every *other* embedded ELF depends on `$(LIB)`
(`libb1nix.a`) / `$(CRT0)` in its `userspace/Makefile` rule, and those are
rebuilt per-arch, so switching `ARCH` forces them to relink for the new target.
`native_smoke` (and `tcc`, `m30_pie`) link **neither** the libc nor crt0 — their
only prerequisite is the `.S`/`.c` source, which does not change across an arch
switch. So after building `ARCH=x86_64` and then `ARCH=x86`, the **leftover
64-bit `native_smoke` binary survived** in the shared build dir, and the
`make ARCH=x86 ... iso` rule `xxd`'d it straight into
`build/x86/initramfs_native_smoke.inc`.

Proof: in the failing build the x86 `.inc` header was
`e_ident[EI_CLASS]=0x02` (ELFCLASS64), `e_machine=0x3e` (EM_X86_64), byte-for-byte
identical to the x86_64 `.inc`, while every other x86 `.inc` (m12/m13/m27) was
correctly `0x01`/`0x03` (ELFCLASS32 / EM_386).

### Why that produced exactly this symptom

- The dispatcher (`kernel/user/process.c`) tries `user_load_elf64` **first**. A
  64-bit ELF passes its `EI_CLASS==ELFCLASS64 && e_machine==X86_64` checks, so it
  is accepted — hence the log line is `ELF load:` (the ELF64 printer) instead of
  `ELF32 load:` for native, and **only** for native. That mismatch was the tell.
- The kernel runs the BSP in 32-bit mode, so the 64-bit code is fetched and
  decoded as 32-bit. Its early bytes happen to decode into a few valid 32-bit
  instructions, then hit an invalid opcode → `#UD` / SIGILL at `eip=0x0200001c`.
  ("Executes through the early bytes then faults" = decoding the *wrong-width*
  binary, not partial frame corruption.)
- "Per-BUILD deterministic, flips with initramfs size" was coincidence: any
  build done right after an x86_64 build inherited the stale 64-bit binary;
  a build preceded by a `make clean` that *also* relinked native got a correct
  32-bit one.

## The fix

`Makefile`: an **arch-guard stamp** in the shared userspace tree,
`userspace/build/.arch`, added to `USERSPACE_DEPS` (so every `*.inc` re-bundles
when it changes). When `ARCH` differs from the recorded value it wipes
`userspace/build` so every output relinks for the new arch, then records the new
arch. Its mtime only advances on a real switch, so same-arch builds don't churn.
The heavy per-triplet ported libs (curl/dropbear/pcre2/openssl/mbedtls) live in
`build/<lib>-b1nix/$(B1NIX_TRIPLET)/` and are untouched by the wipe.

This kills the whole "stale initramfs after an arch switch" pitfall, not just
native_smoke — `tcc` and `m30_pie` had the identical exposure.

Also: `tests/smoke.sh` `TIMEOUT` is now env-overridable
(`TIMEOUT=300 sh tests/smoke.sh x86`) — the full suite's dropbear KEX step is
slow under TCG and can exceed the 120 s default on a loaded host.

## Verification

`TIMEOUT=300 sh tests/smoke.sh x86` → **369/369** (full green), single-CPU and
`-smp 4` both reach `B1NIX-TEST: done`. `NATIVE-SMOKE: ok` + `NATIVE-SMOKE: done`
now print in both passes, and native loads via the correct `ELF32 load:` path.

(At the time this fix landed, `M32B-SSH: ok service-lifecycle` was still failing
because init auto-skipped sshd on 32-bit; that was fixed separately right after —
dropbear works on 32-bit now, so init auto-starts it. See
[`docs/m32b-ssh.md`](m32b-ssh.md).)

---

# Historical investigation (superseded — kept for context)

The original handoff chased a PMM frame double-allocation theory because the
SIGILL looked like "partial corruption" of native's code page. That was wrong:
the page was fine; the *binary* was the wrong architecture. The ruled-out
hypotheses below were all correctly ruled out — they just weren't the cause.

- Identity-map shadow at PD index 8 (0x02000000): correctly ruled out —
  `paging_create_address_space` caps the cloned low identity map at
  `__kernel_end` (~13–16 MB → PD 0-4), so PD 8 stays free for the user mapping.
- Direct-map overflow in the loader copy, spawn sequencing, page-sharing dedup,
  `__kernel_end` drift: all ruled out.

The decisive clue, missed initially, was right in the log: native printed
`ELF load:` while every sibling printed `ELF32 load:`.
