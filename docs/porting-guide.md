# b1nix Porting Guide — how to bring up a new architecture

A reusable playbook distilled from the `ARCH=x86` (i386 / ELF32) port, which
took the smoke suite from **29 → 369/369**. It is organized by *the order things
break in*, not by subsystem, because that is the order you will hit them. Each
section says: what is arch-specific, the failure signature, and the fix pattern.

The concrete worked example is [`docs/x86-32bit-port.md`](x86-32bit-port.md)
(what the x86 port actually changed). The collision post-mortem in
[`docs/native-smoke-collision.md`](native-smoke-collision.md) is the canonical
example of "looks like a kernel bug, is actually a build bug" — read it before
you trust any symptom.

> **The single most important habit:** every marker is guarded by real code.
> When a test fails, trace the marker to the line that emits it, then to the
> syscall/subsystem it exercises, then read that implementation. Never make a
> marker pass without the feature behind it (see `CLAUDE.md` "NO FAKE PASSES").
> Half the hard bugs below were found by reading the serial log *one line at a
> time*, not by guessing.

---

## 0. Ground rules before you start

- **Keep the existing arch green.** All arch divergence goes behind
  `#ifdef __x86_64__ / #else / #endif` (compiler-defined macros, not a custom
  one) or behind `g_*` runtime globals. A shared header with a hard-coded 64-bit
  constant is a latent bug for the new arch. Rebuild the *old* arch after every
  shared-file edit — `make ARCH=x86_64 iso` must stay warning-free.
- **One source of truth for arch identity.** `tools/toolchain-env.sh` maps
  `B1NIX_ARCH → {triplet, gcc-arch, rootfs}`. Every toolchain/port build script
  sources it. Add the new arch *there first*; do not scatter `if [ arch = … ]`
  across scripts.
- **The build tree is shared across arches.** `userspace/build/` and the kernel
  `build/` objects are NOT arch-qualified. Switching `ARCH` without forcing a
  rebuild silently mixes object files of two ABIs. The `Makefile` arch-guard
  stamp (`userspace/build/.arch`) now wipes `userspace/build` on an `ARCH`
  change — keep that working; it is what prevents the
  [native-smoke collision](native-smoke-collision.md) class of bug. For the
  kernel, `make clean` between arches (there is no header-dep tracking).

---

## 1. Toolchain & build system (you can't compile a byte without this)

**Arch-specific:** target triplet, ELF class/machine, `-m` flags, GCC config.

Checklist:
1. `tools/toolchain-env.sh`: add the `B1NIX_ARCH` case (triplet, gcc-arch,
   rootfs). Per-triplet build homes keep x86 and x86_64 objects separate.
2. `tools/patch-gcc.py`: add a target-tuple case so GCC recognizes the new
   `*-b1nix` triplet (and `config.sub` accepts the OS). The x86 port added the
   `i686-b1nix` case here.
3. `userspace/Makefile`: `TARGET`, `LDFLAGS` (`-m elf_i386` vs `-m elf_x86_64`),
   `B1NIX_TRIPLET`, `CFLAGS` (e.g. `-msoft-float` for 32-bit so the freestanding
   libc doesn't emit SSE).
4. **Any `../build/<lib>-b1nix/...` path needs the `$(B1NIX_TRIPLET)` segment.**
   This bit us twice — `PCRE2_DIR` and then `MBEDTLS_DIR` were missing the
   triplet, so the new arch silently linked nothing (m32_nettool fell back to
   HTTP-only, −13 markers). Grep for `build/.*-b1nix` and verify every one is
   triplet-qualified.

**Failure signature:** the link "succeeds" but the feature is silently absent
(a fallback path), or the wrong-arch object is embedded. Diff the produced
binary's ELF header (`EI_CLASS`, `e_machine`) against the arch you think you
built.

---

## 2. The ELF loader dispatch (the binary must even be *recognized*)

**Arch-specific:** `EI_CLASS` (ELFCLASS32/64), `e_machine` (EM_386 vs
EM_X86_64), program-header struct widths.

The kernel tries `user_load_elf64` then `user_load_elf32`
(`kernel/user/process.c`). Each rejects the wrong class/machine. Two traps:
- The new arch needs its own `user_load_elfNN` (32-bit phdr fields are `u32`,
  not `u64`) plus the `userspace ABI base` handling.
- **A wrong-arch binary that passes the *other* loader's checks is the nastiest
  failure mode**: it loads, runs, and faults with a plausible-looking SIGILL.
  The tell is in the log — native printed `ELF load:` (the ELF64 printer) while
  every sibling printed `ELF32 load:`. Keep the two loaders' log prefixes
  distinct so a mis-dispatch is visible at a glance.

---

## 3. ISR / syscall entry assembly (the highest-value, highest-pain area)

**Arch-specific:** register save/restore order, the syscall instruction
(`int $0x80` vs `syscall`), the interrupt-frame layout, segment registers.

This is where the **single biggest x86 bug** lived. On *every* ring-3 return the
segment-register restore (`movw` into ds/es/fs/gs) ran *after* the general
registers were popped, so `popl %eax` was immediately clobbered by the segment
load — corrupting the syscall return value (and any live user EAX on a timer
IRQ). `fork()` returned 0x33 instead of 0 in the child; random user state rotted
on interrupts. Fix: **restore segments BEFORE popping the GP registers**, in
`syscall_entry.S` *and* `isr.S` (`isr_common` and `irq_common`). This single fix
moved ~130 markers (191 → 320).

Lessons that generalize:
- Write down the exact interrupt-frame offsets and put them in the port doc
  (x86: `eax@0 … eip@36 cs@40 eflags@44 esp@48 ss@52`). Every `.S` and the C
  signal/ptrace code must agree byte-for-byte.
- The order of *segment restore* vs *GP-register restore* vs *iret* is load-
  bearing. Get it wrong and the corruption is intermittent and value-dependent
  — exactly the kind of bug that looks like a scheduler or memory bug.
- User segment selectors are arch/mode-specific (x86 32-bit: CS=0x1B, SS=0x23,
  FS=0x38 per-CPU, GS=0x33 TLS). The signal restorer and `sigreturn` must use
  them.

---

## 4. Memory map & paging (everything above touches this)

**Arch-specific:** virtual layout, paging depth, direct-map base, identity-map
extent, MMIO mapping.

Define the whole map up front in `mm.h` behind `#ifdef`, and write it into the
port doc. x86 32-bit:

| Region        | Address                          |
|---------------|----------------------------------|
| User ELF base | `0x02000000` (PD 8)              |
| User stack    | `~0xBFFFxxxx` (PD 766–767)       |
| `USER_SPACE_LIMIT` / stack top | `0xC0000000` (PD 768) |
| Direct map    | `DIRECT_MAP_BASE 0x80000000`, ≤1 GB |
| Kernel heap   | `KHEAP_START 0xC0000000`         |
| klarge arena  | `0xE0000000 – 0xFF000000`        |

Pitfalls that bit us:
- **The user ELF base can collide with the cloned identity map.**
  `paging_create_address_space` clones the kernel's low identity map; clone *too
  much* and the huge-page identity PDE at the user base (PD 8 = `0x02000000`)
  shadows the loader's 4 KB user mapping → the binary runs from identity-mapped
  physical garbage. Cap the cloned identity entries at `__kernel_end` (x86:
  PD 0–4, ~16 MB), leaving the user-base PD free. (This was *correctly ruled
  out* for native-smoke — but it is a real, separate bug we fixed earlier.)
- **Clone/free the correct user PD range.** Fork CoW (`paging_clone_address_
  space`) and teardown (`paging_free_address_space`) must walk PD `0..768` so
  the downward-growing user stack (PD 766–767) is included. Cloning only `0..511`
  handed children a fresh zero stack → `ret` to 0 → SIGSEGV.
- **MMIO BARs above the direct map.** PCI BARs (AHCI/NVMe) can sit above the
  direct-map ceiling; map them via `vmm_map_mmio` rather than assuming
  `phys + DIRECT_MAP_BASE` is valid.
- **klarge / heap window offsets are arch-sized.** A 64-bit `+64 GB` offset
  reused on 32-bit aliased the kheap and clobbered the boot task.

When a user page faults but is *mapped*, suspect a wrong physical frame or a
shadowing PDE, not the loader copy.

---

## 5. SMP / per-CPU bring-up

**Arch-specific:** AP trampoline, per-CPU GDT/segment setup, `current_task`
location.

- **Build each AP's per-CPU GDT on the BSP *before* the SIPI.** The x86 AP
  trampoline does `lgdt g_cpu_gdt_ptrs[cpu]` and loads `FS=0x38` before setting
  its ready flag — but those descriptors were being built by code running *on
  the AP*, after the trampoline, so the AP loaded a null GDT and silently
  triple-faulted; the BSP hung at "sending SIPI". Split "build descriptor data"
  from "load it"; the BSP fills every AP's GDT before waking it.
- **Mind which tasks may run on an AP.** The 32-bit port hit a fork/waitpid
  deadlock: an AP CAS-claimed a freshly-forked userspace child whose first
  ring-3 syscall couldn't make progress against the Big Kernel Lock while the
  parent blocked in `waitpid` → the parent never reaped → wedge. Fix:
  `ap_runnable = (image->kind == USER_IMAGE_ELF64)` in `user_spawn` — naturally
  per-arch (ELF32 → BSP-only, ELF64 → still on APs), and APs still run stealable
  workers so work-stealing is unaffected. Generalize: if the new arch's
  userspace can't yet make forward progress on an AP, keep it on the BSP and
  revisit, rather than chasing the deadlock under a half-working ABI.

---

## 6. Types, time, and struct widths

**Arch-specific:** `long`/pointer width, how 64-bit values cross the syscall
boundary, `sigset_t` / `sa_flags` width, stack-arg alignment.

- **64-bit values over a 32-bit syscall ABI must be split lo/hi explicitly.**
  `utime` truncated past 2038 because libc cast the `time64` to `(long)` before
  the `int $0x80`. Split into two 32-bit args and reassemble in the kernel.
  (b1nix time is full 64-bit end-to-end now — keep it that way.)
- **Stack alignment before `call` differs** (i386 wants `%esp & 0xF == 4` at
  entry to `main` after the return address is pushed; x86_64 wants `== 8`). Get
  the crt0/argv-stack builder right or C code with SSE/aligned locals faults.
- **Audit shared structs for 64-bit-only fields** (`sigset_t` as `u64`,
  `sa_flags`, coredump ELF offsets). The coredump checker needed ELF32/EM_386
  offsets.
- **The `syscall(...)` wrapper macro is arch-specific** (i386 needs an EBP
  trick: a constant-0 sixth arg keeps the frame pointer usable).

---

## 7. Drivers, net, and the long tail

These are mostly arch-neutral *if* the layers above are correct — most "driver"
failures on a new port are really paging (MMIO) or DMA-address bugs. Two real
ones worth flagging:
- **DMA buffers that straddle a page boundary** map to non-contiguous frames;
  scatter-gather split at 4 KB. (This caused a page-fault-at-mount that *looked*
  like a PMM/RAM-size bug — see
  [`project_virtio_blk_dma_straddle`].)
- **Net tests hang on a live host network.** The smoke harness forces
  `restrict=on` in QEMU usernet so external probes can't connect to the real
  internet and stall the suite. Keep that deterministic.

---

## 8. Debugging methodology (how to actually find these)

1. **Read the full serial log, top to bottom, before changing anything.** The
   last successful marker + the first silence/PANIC localizes the subsystem. The
   two costliest bugs (eax-clobber, native-smoke) were each visible in the log
   to anyone reading carefully — a corrupted return value, and `ELF load:` vs
   `ELF32 load:`.
2. **Beware the observer effect.** On fixed-load binaries (TCC, native_smoke)
   any added `console_write` shifts kernel `.text` and changes layout-sensitive
   outcomes. Prefer a cmdline-gated one-shot probe, or the QEMU HMP monitor /
   gdbstub with a physical-address watchpoint, over scattered prints.
3. **Distinguish "kernel bug" from "build bug" early.** Diff the actual embedded
   bytes (`.inc` ELF header, the on-disk binary) against what you intended. A
   surprising fraction of "port bugs" are stale/wrong-arch artifacts in the
   shared build tree.
4. **Host TCG is slow and the suite has a per-pass wall-clock timeout.** Running
   builds and suites back-to-back can push one QEMU pass past the default 120 s
   (`tests/smoke.sh` `TIMEOUT`, now env-overridable). Symptom: a flood of
   "missing marker" fails with **no** panic and a `terminating on signal 15`
   line. Use `TIMEOUT=300` on an idle host for a representative number; don't
   chase phantom regressions caused by the timeout.
5. **No fake passes, work the root cause.** A marker like
   `M14-SMOKE: ok ext4-persistence` must mean the write→sync→umount→remount→
   read-back actually verified correct data.

---

## 9. Definition of done

- `make ARCH=<new> KERNEL_CMDLINE="b1nix.test=1" iso` builds warning-free.
- `TIMEOUT=300 sh tests/smoke.sh <new>` is green: single-CPU **and** `-smp N`
  both reach `B1NIX-TEST: done`, zero failures.
- `make ARCH=x86_64 iso` (and its smoke) still green — no regression to the
  reference arch.
- The port has its own `docs/<arch>-port.md` (memory map, frame offsets, ABI
  selectors, the bug fixes with commit hashes) and the roadmap line is updated.
- Run `/graphify --update` and commit the refreshed graph as part of closeout.

The x86 port reached all of these (369/369, single + `-smp 4`). Use it as the
reference for what "done" looks like.
