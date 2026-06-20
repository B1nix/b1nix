# V8 → b1nix GN target — skeleton (validated patch plan)

**Status: GN-target skeleton only. The full port is the 2–3 month effort in
[`docs/v8-feasibility.md`](../../../docs/v8-feasibility.md). This file is the
*proven* set of edits that adds `b1nix` as a GN/V8 `target_os`** — the dominant
blocker from the probe — so the path is mapped before the multi-GB `fetch v8`.

## 🚧 JIT PORT (in progress) — enabling V8's JIT on b1nix

The shipped d8 is **jitless** (interpreted Ignition only). Next milestone: the
JIT pipeline (Sparkplug baseline + TurboFan optimizing). Prep + kickoff:

**Prerequisite audit (b1nix x86_64):**
- **Executable memory: already works.** mmap'd pages are executable by default
  (the kernel sets NX = `VMM_NO_EXECUTE` bit 63 only when explicitly asked; mmap
  never does). b1nix already runs dynamically-generated machine code — TCC (M25)
  JITs and executes C in-VM. V8's `OS::SetPermissions` (platform-posix.cc) cycles
  `mprotect` RW↔RX↔RWX; on b1nix's permissive model every one resolves to an
  executable page, so JIT code runs. **No kernel change needed to get JIT working.**
- **W^X / NX hardening: NOT enforced** (pages V8 marks RW/R-only stay executable).
  `sys_mprotect` ignores `PROT_EXEC` and never sets NX; `PROT_NONE` reserves stay
  readable (guard pages don't trap). Functional-but-not-hardened — a security
  follow-up, not a JIT blocker. Tighten only if the run-chase shows V8 depending
  on a real guard-page trap.
- **I-cache coherence: free on x86_64** (coherent; no explicit flush). (ARM would
  need `__builtin___clear_cache`.)
- **Signals: work** (SIGSEGV/handlers proven during the jitless bring-up).
- **mksnapshot: host-built.** gn builds mksnapshot under the `clang_x64` (host)
  toolchain and runs it on Linux to emit the embedded snapshot + x64 builtins, so
  the cross setup is fine for JIT too (confirmed: the JIT `ninja` builds
  `clang_x64/.../libc++` for the host mksnapshot).

**Build:** `tools/v8-gen-jit.sh` → a SEPARATE `out/b1nix-jit` (keeps the proven
jitless `out/b1nix` as a fallback). Flips `v8_jitless=false` +
`v8_enable_sparkplug=true v8_enable_turbofan=true` (TurboFan mandatory when not
jitless), `v8_enable_maglev=false` + `v8_enable_webassembly=false` to keep the
surface down. JIT compiles+links cleanly first try — the jitless port already
paved the whole libc surface (3003 targets PC-on / 2446 PC-off; relink with
`sh tools/v8-link-d8.sh b1nix-jit`). Run via the default ISO built with
`b1nix.v8jit` (kernel hook drops `--jitless`; also passes `--single-threaded`
during bring-up) + the `v8-jit-ext4.img` disk (a copy of v8-ext4.img with the JIT
d8 swapped in).

**🎉 JIT RUNS JAVASCRIPT (PC-off): 5/8 m58.js markers, ZERO corruption.** Real
Sparkplug/TurboFan executes JS on b1nix — hello + loop-sum (100k JIT'd loop) +
array-reduce + object-sort + json. Three real OS bugs fixed to get here (all ship
in ISO, v0.58.3):
1. **Aligned allocators were stubs** (`posix_memalign`/`aligned_alloc`/`memalign`
   ignored alignment → just malloc/16B). V8 needs the Isolate page-aligned
   (`AlignedAlloc(sizeof(Isolate), 4096)`) so `isolate_data_` is 64-aligned
   (`Check failed: IsAligned(...)`). Rewrote as real over-aligned allocation with
   a sentinel + raw-ptr stashed below the payload so plain free() recovers it;
   the sentinel's bit 0 is clear, which a live boundary-tag header never is.
   `realloc`/`malloc_usable_size` made aligned-aware too (they were reading the
   sentinel as a size → heap corruption). userspace/libc/stdlib.c.
2. **PROT_NONE mmap allocated frames eagerly** → V8's cage/DecommitPages churn
   drained+corrupted the PMM free list. Fixed: PROT_NONE anonymous mmap uses the
   lazy (no-frame) path. kernel/syscall/syscall.c sys_mmap.
3. **🎉 Pointer compression WORKS with the CODE CAGE off** (the memory win for
   b1nix — compressed 32-bit data-heap pointers). The bug was specifically the
   *external code space* (the separate **code** cage): on b1nix its base comes up
   ZERO, so JIT'd/builtin code pointers decompress to near-null and the first
   indirect call jumps to ~0x400 → crash at isolate init. The data cage was fine
   all along. Root-caused by gdb post-mortem (serial showed the original PC crash
   `rip=0x400 cr2=0x400` = jump-to-near-null; jitless was immune only because
   `v8_jitless=true` emits NO JIT code, so its code cage is never exercised).
   **FIX:** `v8_enable_pointer_compression=true v8_enable_external_code_space=false`
   in `v8-gen-jit.sh`. Verified: a PC+no-ECS d8 (out/b1nix-jit-pcdata) runs the
   full 13-marker m58.js suite to completion under Sparkplug (`--no-opt`), 0
   crashes. (TurboFan tier-up still crashes separately — see below — independent
   of PC; the working JIT config remains Sparkplug + this PC setting.)

**🎉 SPARKPLUG BASELINE JIT FULLY WORKS — the COMPLETE m58.js suite (13 markers).**
With `--no-opt` (TurboFan off, Sparkplug baseline JIT only) d8 runs the entire test
to completion, ZERO crashes/corruption, ~90s: hello + loop-sum (100k) + array-reduce
+ object-sort + json + gc-churn (30k objects) + recursion (fib 25) + **closure +
try/catch + Map/Set + Int32Array typed-array + string split/join + regex** + done.
Both jitless (interpreter) AND Sparkplug pass the full 13-marker suite. So a broad,
realistic JS feature set works under the baseline JIT on b1nix. The kernel hook runs
`d8 --single-threaded --no-opt /mnt/v8/m58.js`; the smoke v8 instance checks all 13.

**TurboFan layer 1 — the #MF was a real b1nix FPU bug, now FIXED (v0.58.4).**
The earlier post-mortem analysis (below) was MISLED by a clobbered core and
wrongly dismissed FP as "FPU masked FCW=0x037F / #MF-on-`ret` is a misdecode."
A live QEMU-gdbstub session (`qemu … -gdb tcp::1234 -S`, host gdb,
`hbreak` on the `fldenv` in `nearbyint+32`) showed the FIRST fault under
TurboFan is a **#MF (x87 FP exception, vec 0x10) inside libm `nearbyint`**:
`nearbyint` does `fegetenv` → `rint` → `fldenv -0x28(%rbp)`, and the saved env
it reloads has **FCW=0x0000 (all x87 exceptions UNMASKED) + FSW=0x0001 (Invalid
pending)** while the live FPU was the correct 0x037f. So the control word had
been zeroed; `fldenv` unmasks the pending Invalid → #MF → V8 `OS::Abort`.

Root cause: **fork copied a stale/zero FPU save-area.** `exec`
(`arch_fpu_init_current`) resets the *live* FPU to the masked default but never
flushed the task's `fpu_state` buffer, which for a fresh slot is the kzalloc'd
zero (FCW=0x0000). `scheduler_fork_current`'s `memcpy(child, parent, sizeof
task)` then copied that zero buffer + `fpu_initialized=1` into the child, so the
child restored an unmasked control word. Fix (two parts): (1) flush the clean
FPU into the save-area right after `arch_fpu_init_current` at exec
(`kernel/user/process.c`); (2) snapshot the parent's *live* FPU into the child
at fork (`arch_fpu_save(child->fpu_state)` in `kernel/sched/scheduler.c`) for
correct POSIX FP inheritance. This is a general kernel bug — any forked process
doing FP could inherit a zeroed FCW, not just V8. Verified: the #MF is gone,
full smoke green (only the v8-instance's by-design M14-nvme/M27-cmdline noise).

**TurboFan layer 2 — the NEXT chase (open): real per-thread ELF TLS.** With the
FPU fix, d8 TurboFan-ON runs *past* nearbyint and faults deeper: a **page fault
at cr2=0x0** (err=0x5, user read). The fault `rip=0x32864a1` is in the STATIC d8
binary (0x2000000–0x347ffff, NOT JIT'd code), so it disassembles directly
(`x86_64-b1nix-objdump -d --start-address=… build/v8-out/d8-jit.b1nix`). The
faulting instruction is:

    32864a1:  64 48 8b 1c 25 00 00 00 00   mov %fs:0x0,%rbx   (in v8::base::OS::GetCurrentThreadId)

i.e. a **`thread_local` read** (V8 caches its tid in a thread_local; the fn then
reads `-0x18(%rbx)`/`-0x10(%rbx)`). `cr2=0x0` ⇒ **FS base is 0** for this thread,
so the access lands at linear 0. Root cause is architectural: **b1nix libc
`pthread.c` does NOT implement real ELF TLS for spawned threads** (see its own
comment, "b1nix doesn't compile with ELF TLS sections… we don't expose
thread-local-storage to user code"): `pthread_create` clones WITHOUT
`CLONE_SETTLS` and the bootstrap just `SYS_SET_TLS(st)` parks the `pthread_state`
pointer at `%fs:0`. But V8 uses genuine `thread_local` variables — the very
`.tdata/.tbss` COMDATs the `linker-cxx.ld` fix collects — and its worker/platform
threads (present even under `--single-threaded`) read them. Only the MAIN thread
gets a proper variant-II TLS image (set up by the ELF loader, process.c:~1549);
spawned threads get no per-thread `.tdata/.tbss` copy ⇒ thread_local read faults.
V8's SIGSEGV handler *also* calls GetCurrentThreadId (thread_local) ⇒ re-faults ⇒
**d8 hangs** instead of aborting. This is the same "background threads hit
near-null" symptom noted earlier.

**Layer-2a DONE — real per-thread ELF TLS (commit 4232fc7, v0.58.4).** Added
`SYS_GET_TLS_INFO(161)` (exposes the running image's PT_TLS template) and made
`pthread_create` build a variant-II TLS block per thread (mirroring the loader's
main-thread layout: `[.tdata|.tbss][TCB]`, self-pointer at `%fs:0`, install as
the FS base; probe mutex-guarded for concurrent spawns; munmap'd at join/detach).
Verified `M29-PTHREAD: ok thread-local` (4 pthreads, per-thread `__thread`, no
bleed) on boot+v8 instances; x86_64 smoke 804/0 OS-green. Safe for existing
pthread users (libc never reads `%fs:0` — `pthread_self` uses SYS_GETTID).

**Layer-2b — the SECOND root cause: `user_jump.S` clobbers the FS base at ring3
entry.** Even after per-thread TLS, a relinked d8 (new libc) faulted at the SAME
`mov %fs:0` in GetCurrentThreadId — but on the MAIN thread (rsp = `USER_STACK_TOP
− 2032`; worker stacks mmap at ~4 GB via `vm_find_free_area`). Kernel trace
(`DBG LTLS`/`STLS`) proved the loader DID set main's FS base (`tp=0x7fffff7fe0b0`)
yet main ran with FS base 0 and NO intervening SYS_SET_TLS. Root cause:
`x86_user_jump` does `mov %ax,%fs` (user-data selector) on the way to ring3, and
**in 64-bit mode loading a segment selector ZEROES that segment's base**, wiping
the IA32_FS_BASE MSR the loader just programmed. The code already skips `%gs` for
exactly this reason; it wrongly reloaded `%fs`. The scheduler restores the base
on the next context switch, so a thread only faults if it reads a `thread_local`
BEFORE its first preemption — my libc change shifted d8's timing to expose it
(pre-existing latent bug; affects any TLS-before-first-switch program). Fix
(`user_jump.S`): snapshot the FS base (`rdmsr 0xC0000100`) before the selector
load and restore it (`wrmsr`) after — keep the selector correct (arch_set_fs_base
relies on `%fs` pointing at the user-data descriptor, per arch.c) AND the base
intact. TWO gotchas: (1) naive "just drop `mov %ax,%fs`" is WRONG — leaving a
non-user `%fs` selector breaks userspace. (2) `rdmsr`/`wrmsr` clobber eax/edx/ecx
which ALIAS argc(`%rdx`)/argv(`%rcx`) — must stash BOTH in scratch (r10/r11)
around the MSR ops, else `main` gets a garbage argc (= FS-base-high = `0x7fff…`
for a TLS program → mis-parses args → hangs). This was the real cause of the
"hang" first misdiagnosed as layer-2c.

**Layer-2b VERIFIED: the FS-base+argc fix CRACKS the GetCurrentThreadId crash.**
With #MF + per-thread TLS + the `user_jump.S` fix, the relinked d8 **runs the
full Sparkplug suite (`M58-V8: ok hello … ok recursion … done`, 0 faults)** on
`-smp 1` in test mode, and under TurboFan reaches `M58-V8: ok hello` before the
next (separate) crash. (Standalone non-`test=1` boots STARVE d8 — getty/bash hog
the single CPU — always run d8 with `b1nix.test=1` so the rc keeps the scheduler
busy. And use `-smp 1` to avoid the SMP issue below.)

**Layer-2c — OPEN #1: SMP `tlb_shootdown timeout` panic (REAL, deterministic).**
Under `-smp 2`, d8 TurboFan panics `[PANIC] tlb_shootdown timeout` — one CPU
doesn't ACK the shootdown IPI within the 2^28-spin guard (tlb.c:172). CONFIRMED a
real kernel bug, not host oversubscription: reproduces on an IDLE box 2/2 runs,
and `-m 4096` (4 GB, ~no eviction — only 2 swap lines) ALSO panics, so it is NOT
the eviction/swap shootdown (eviction.c) nor memory-pressure — it's d8's regular
mmap/munmap unmap shootdowns (paging.c). `-smp 1` avoids it entirely
(`online_cpu_count()==1` short-circuits dispatch). Ruled out: lock-contention
deadlock — `spin_lock`/`spin_lock_irqsave` already poll `tlb_shootdown_poll()`
(spinlock.h:44), and the AP idle loop is `sti; hlt` (IRQs on, would ACK). So the
stuck AP is irqs-off in a NON-lock-spin region for >2^28 spins. **DIAGNOSED
(v0.58.5): added a timeout diagnostic** (tlb.c names the non-ACKed CPU + its
`current_task` via new `percpu_cur_task()` in lapic.c). It prints e.g. `tlb:
STUCK cpu 1 task=d8 (initiator cpu 0)` — and the stuck task is a USERSPACE task
(d8 on one run, the concurrent `m32-smoke` on another, op=PAGE and op=ALL), NOT a
kernel thread. So a userspace process is sitting in a long irqs-off kernel
critical section that neither receives the IPI nor polls. It is during d8's
compute phase, not disk I/O. STILL OPEN — pinning the exact site needs the stuck
CPU's live RIP (an NMI capture or a per-CPU "irqs-off entry" breadcrumb, since
`current_task->context.rip` is only the last switch-out point). **FIXED
(v0.58.5): the stuck site was `sys_mmap`.** A temporary per-CPU current-syscall
breadcrumb pinned the non-ACKing CPU to `syscall=58 (SYS_MMAP)` — d8's large JIT
cage / NORESERVE mmaps walk thousands of pages in `sys_mmap`'s page-mapping loops
(`vmm_set_lazy`+`paging_mprotect_page`, and the anonymous-with-frames loop), all
with no chance to take the shootdown IPI, so an initiator on another CPU spun to
its 2^28 guard. FIX: call `tlb_shootdown_poll()` once per page in both
`sys_mmap` loops (syscall.c) — a single load when nothing is pending. VERIFIED:
`-smp 2` now runs d8 with **0 `tlb_shootdown timeout` panics** (2/2 runs), and
`-smp 1` still completes `ok hello → ok recursion → done`; full smoke 812/0/0.
Repro: TurboFan hook (drop `--no-opt` main.c:591), `b1nix.test=1 b1nix.v8run`,
`-smp 2`, relinked `d8-jit-tls.stripped`. **The shipping default (`--no-opt`,
Sparkplug) now runs the full suite on `-smp 2`** — `ok hello → ok recursion →
done`, 0 timeouts, only the benign post-`done` dead0000 teardown (identical to
`-smp 1`). So multi-CPU Sparkplug V8 works.

**Layer-2c OPEN #1b — RESOLVED (was an ARTIFACT): `-smp 2` TurboFan runs fully.**
An earlier diagnostic showed `-smp 2` TurboFan aborting at `cr2=0xdead0000`
before any marker, suspected a V8 `CHECK` under SMP. It did NOT reproduce under
controlled conditions — it was a stale/half-written d8 disk and/or host
oversubscription during the deadlock investigation (4 smoke VMs + many extra d8
qemu on 8 cores). With the committed kernel (mmap poll), an idle box, and a
freshly verified d8 disk, **`-smp 2` TurboFan runs the full 12-test m58.js suite
`ok hello…recursion(fib25 tier-up)…string-regex…done`, 3/3 runs, 12/12 each, 0
faults** (no dead0000, #EXC, or timeout); `-smp 1` TurboFan identical. So
multi-CPU TurboFan V8 works — no kernel fix was needed beyond OPEN #1's mmap
poll. Tier selection is now a clean cmdline flag: `b1nix.v8opt` drops `--no-opt`
(TurboFan) vs the default Sparkplug; repro `b1nix.test=1 b1nix.v8run b1nix.v8jit
b1nix.v8opt`, `-smp 2`, d8-jit-tls disk.

**Layer-2c #2 — FIXED (v0.58.4): the nearbyint `#MF` was a STUB `fegetenv`.**
d8 TurboFan hit vec 0x10 at `rip=0x32f7840` (`fldenv` in libm `nearbyint`) on the
main thread. nearbyint does `fegetenv(&env)` → `rint` → INLINE `fldenv -0x28(%rbp)`
over the 28-byte x87 env. Pinned by elimination: kernel FPU-save trace never fired
(not a context-switch bug), `FE_DFL_ENV` is correct (`__INITIAL_FPUCW__=0x037F`),
and the relinked d8 has ZERO `fldcw`/`fldenv`/`fxrstor`/`fninit` except nearbyint's
own — so nothing sets FCW. Disasm of `fegetenv` (0x3306120) showed the smoking
gun: **`movl $0x0,(%rdi); ret`** — a STUB (`userspace/libc/unistd.c`) that wrote a
ZERO control word (FCW=0, all x87 exceptions unmasked) + left the rest of the env
as stack garbage, instead of doing a real `fnstenv`. So nearbyint's inline fldenv
reloaded FCW=0 + garbage → #MF. (`fesetenv`/`feholdexcept` were stubs too.) FIX:
implement them with real `fnstenv`/`fldenv`/`stmxcsr`/`ldmxcsr`, and grow `fenv_t`
from 4 bytes (`{unsigned int __cw}`) to the real 32-byte x86_64 layout (28-byte
x87 env + 4-byte mxcsr) — openlibm's nearbyint already uses that layout, and the
old 4-byte struct would overflow once fegetenv writes a full env. VERIFIED: d8
TurboFan runs `ok hello → ok recursion(fib25) → done`, 0 #MF, `-smp 1`. NOTE: a
benign `#EXC cr2=0xdead0000` (V8 exit-teardown poison write) fires AFTER `done` on
both Sparkplug and TurboFan — JS already complete; minor follow-up, not a crash.
GOTCHA: the `fenv.h` change forces a full Mesa rebuild (Mesa includes fenv).

---
*Historical (pre-fix, partly wrong) post-mortem, kept for the method:* a b1nix
M35 core (`coredump_write` → `/mnt/v8/core`, `debugfs -R "dump /core out.core"`,
host `gdb d8-jit.b1nix out.core`) put the apparent fault at
`DefaultForegroundTaskRunner::deque::_M_push_back_aux`, `rip=0x329375b`
mid-instruction — read as "control-flow corruption." That core was from a
*later/secondary* fault with a stack clobbered by V8's own SIGSEGV handler, so
the deque/`#GP` story was a red herring; the real first fault is the #MF above.
A boundary-tag libc validator (header==footer on every free) emitted **zero
`HEAP-CORRUPT` markers** through the fib(25) compile, correctly ruling out libc
heap corruption. (All that instrumentation was reverted, not committed.)

## ✅✅✅ `d8` COMPILES AND LINKS for b1nix (full V8 engine)

`d8.b1nix` = 22 MB `ET_EXEC` `EM_X86_64`, entry `0x2000000`, **0 undefined refs**.
The whole engine compiles with `x86_64-b1nix-g++` and links via
**`tools/v8-link-d8.sh`** (gn's own link uses bare g++ + empty sysroot → wrong;
the script relinks gn's `out/b1nix/d8.rsp` object set with the b1nix recipe:
ranlib thin archives + `ld -T userspace/linker-cxx.ld crt0.o --start-group @d8.rsp
--end-group` + libstdc++/libsupc++/libgcc + openlibm `libm.a` + whole-archived
`libb1nix.a`). Link-chase fixes: `__stack_chk_*`, extern-C malloc/sched, openlibm
math, **Patch 17 abseil `config.h` `ABSL_HAVE_MMAP`** (else `LOW_LEVEL_ALLOC_MISSING`
drops per_thread_sem/thread_identity/LowLevelAlloc → 8 refs), `--start-group` wrap.
Gotcha: gn uses `-MMD` (no sysroot-header dep tracking) → re-staging sysroot
headers won't rebuild dependents; `rm` the stale `.o` (once.o, marker.o) + rebuild.

### ✅✅✅ RUN PHASE DONE — d8 RUNS JAVASCRIPT ON b1nix (v0.58.2)

`M58-V8: ok hello` — real V8 `d8 --jitless -e 'print(...)'` boots, deserializes
its embedded snapshot, inits the isolate/heap, and **executes JS** under
QEMU/x86_64. Serial log: `ELF load: /mnt/v8/d8 entry=0x2000000` →
`v8: d8 spawn result: 603` → `M58-V8: ok hello`, zero SIGSEGV.

**Reproduce:**
1. `make ARCH=x86_64 KERNEL_CMDLINE="b1nix.test=1 b1nix.v8run" iso`
2. `sh tools/v8-run-qemu.sh` — attaches `build/v8-out/v8-ext4.img` as AHCI sata0,
   greps the serial log for `M58-V8: ok hello`.

The kernel hook (`kernel/main.c`, guarded by `b1nix.v8run`) mounts sata0 →
/mnt/v8 then `user_spawn("/mnt/v8/d8", {"d8","--jitless","-e",
"print('M58-V8: ok hello')"})`. d8 ships on the ext4 disk (13 MB, too big for the
xxd initramfs). Relink: `tools/v8-link-d8.sh`; restage into image:
`debugfs -w -R "rm /d8" img; debugfs -w -R "write d8.stripped /d8" img`.

**THE runtime bug — TLS variable overlap (root-caused + fixed).** d8 first
SIGSEGV'd in `v8::internal::Isolate::Enter()` at `mov %fs:0,%r12; mov
-0x8(%r12),%rbp; mov (%rbp),%rax` — the TLS slot read back the *thread id*
(0x25b = pid 603) instead of a pointer. Cause: `userspace/linker-cxx.ld` had **no
`.tdata`/`.tbss` rule**, so ld placed each of V8's dozens of COMDAT
`.tbss.<mangled>` sections as an orphan and **overlapped them all at one address**
(readelf: sections 14–24 all at the same VA; `thread_id`, `g_current_isolate_`,
`g_current_per_isolate_thread_data_` all at ti=0x18). V8 caching the tid in
`thread_id` stomped `g_..._thread_data_` → bad pointer → fault. Fix: add
`.tdata : { *(.tdata .tdata.*) }` + `.tbss : { *(.tbss .tbss.*) *(.tcommon) }`
so each thread_local gets a distinct PT_TLS offset (now 0x40/0x48/0x50; memsz
0x83→0x98). The loader's variant-II TLS setup (`process.c:1549`) was correct all
along — the *linker script* was the bug. (Plain `linker.ld` has the same latent
gap but survives because its few C binaries have ≤1 TLS var.)

**Two libc gaps surfaced building the (orthogonal) graphics ports for the ISO,
fixed (ship in ISO):** `dladdr`/`Dl_info` (dlfcn.h + stdlib.c stub returning 0 =
"not found", correct for static-only ELF; Mesa `util/build_id.c` needs it) and
`FUTEX_BITSET_MATCH_ANY 0xffffffff` in `<linux/futex.h>` (Mesa `util/futex.c`).
Build-tree hygiene: parked agent worktrees had baked absolute
`.claude/worktrees/agent-XXXX/` paths into the shared `build/` `.la`/`.pc`/
Makefile/CMakeCache files; collapse with
`sed -i -E 's|/b1nix/\.claude/worktrees/agent-[a-z0-9]+/|/b1nix/|g'` over text
files + purge contaminated CMakeCache dirs. (Mesa also needed `build-mesa.sh`'s
`ninja -k 0` to finish `libmesa_util.a` after the intentional libOSMesa.so fail.)

#### Original run-phase plan (kept for reference)
Artifacts preserved at `build/v8-out/` (`d8.b1nix`, `d8.stripped` 13 MB,
`v8-ext4.img` = ext4 with d8+hello.js, `d8.rsp`).
- Expected runtime chase: main-thread **TLS** (✅ was the linker bug above),
  embedded **snapshot** deserialize (`v8_use_external_startup_data=false` — worked
  first try), isolate/heap init (worked once TLS fixed).

## ✅✅ `ninja v8_libbase` BUILDS for b1nix (real cross-GCC, 41 ELF64 objects)

After a full `gclient sync` + `gn gen target_os=b1nix … v8_jitless=true`, the
first ninja target — `v8_libbase`, the platform/base layer — **compiles and
archives** (`AR obj/libv8_libbase.a`, thin archive of 41 `x86_64-b1nix-g++`
ELF64 objects). This is the PORT-PLAN's "compiles v8_libbase = days" milestone.

**`gn gen` args** (in `tools/sync-v8.sh`): jitless ⇒ all JIT/Wasm tiers off,
`v8_enable_temporal_support=false` (Temporal is Rust), `v8_enable_sandbox=false`
(needs libc++ hardening), `use_custom_libcxx=false`, **`is_clang=false`** (the
critical one — GCC build of V8; without it the toolchain rules inject clang-only
`-Xclang`/module/raw-ptr-plugin flags). GN-graph patches surfaced: **Patch 7**
(rust.gni b1nix `rust_abi_target`) + **Patch 8** (clang/BUILD.gn clang_rt dir) —
both alias b1nix to linux to clear gen-time asserts the `coverage`/`clang`
default-configs trigger. The Patch-2 toolchain file needed the real cross path
(`//../../x86_64-b1nix/cross/bin/x86_64-b1nix-`).

**Compile chase = Patches 9–13** (in apply.sh) + **b1nix libc additions** (real
OS improvements, committed in userspace/, shipped from v0.58.1):

| Break | Fix |
|---|---|
| `__has_warning` clang-only | Patch 9: GCC fallback in macros.h |
| `<linux/auxvec.h>`/`<sys/auxv.h>` (cpu.cc, cpu-x86.cc) | Patch 10: guard, x64 doesn't use HWCAP |
| PKU (`pthread_getattr_np`/`PROT_GROWSDOWN`) | Patch 11: V8_HAS_PKU_SUPPORT off for b1nix (jitless) |
| absl `<link.h>` ELF symbolizer | Patch 12: ABSL_HAVE_ELF_MEM_IMAGE off for b1nix |
| absl `std::wcslen` | Patch 13: `::wcslen` + `<wchar.h>` |
| llvm-libc math: `math_errhandling`,`FP_ILOGB0`,… | math.h C99 macros |
| `fegetexceptflag`/`fesetexceptflag` | fenv.h + libc stubs |
| `prctl` (thread/VMA naming) | new `<sys/prctl.h>` + no-op libc `prctl()` |
| si_code (`BUS_*`,`FPE_*`,`ILL_*`), `si_addr` | signal.h |
| `struct tm` `tm_gmtoff`/`tm_zone` | time.h |
| `malloc_usable_size` | malloc.h + stdlib.c |
| `pthread_attr_getstack`, `pthread_getattr_np` | pthread.h/.c (via /proc/self/maps + current SP) |
| `PTHREAD_STACK_MIN` | pthread.h |
| `__NR_gettid` | syscall.h alias to SYS_GETTID |
| `strerror_r` | string.h + libc (XSI) |
| 17 missing errno (`ENOLINK`,`ECANCELED`,…) | errno.h (Linux ABI values) |
| `std::strtoll`/`atoll`/`lldiv`/`strtold` | C99 stdlib: add atoll/lldiv/lldiv_t/strtold/_Exit + flip libstdc++ `_GLIBCXX11_USE_C99_STDLIB` |

**Machine-local toolchain staging (not committed; redo after a toolchain rebuild,
or build-toolchain.sh will pick most up once the headers are complete):**
1. Copy edited `userspace/include/*` into `…/x86_64-b1nix/cross/x86_64-b1nix/include/`.
2. **Also** copy `stdlib.h` (and any fixincludes header) into
   `…/cross/lib/gcc/x86_64-b1nix/13.2.0/include-fixed/` — GCC's fixincludes
   snapshot shadows the sysroot copy.
3. In `…/c++/13.2.0/x86_64-b1nix/bits/c++config.h` set
   `#define _GLIBCXX11_USE_C99_STDLIB 1` (libstdc++ was built before b1nix had
   the C99 stdlib funcs; a fresh toolchain build now auto-detects them).

**Next:** `ninja mksnapshot` (host toolchain) then `ninja d8` (the bulk — links
all of V8 against libb1nix.a). Goal: `d8 --jitless` prints `print("hello")`.

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

Two scripts drive it (both "run-it-yourself" — they fetch/build external code,
which Claude can't do unattended):

```sh
sh tools/build-gn.sh    # once: builds gn  (cached, survives make clean)
sh tools/sync-v8.sh     # depot_tools + gclient sync (multi-GB) + apply + gn gen
```

`tools/sync-v8.sh` does: clone depot_tools → write a `managed:False` `.gclient`
(so gclient leaves our shallow v8 git alone and only syncs the DEPS sub-trees) →
`gclient sync` → **`tools/patches/v8/apply.sh`** → `gn gen out/b1nix`. Then the
manual chase loop:

```sh
ninja -C build/toolchain_build/v8-skeleton/v8/out/b1nix v8_libbase   # smallest unit; exercises the platform layer
ninja -C ...                 mksnapshot   # host toolchain; proves the host/target split
ninja -C ...                 d8           # the goal: jitless d8
```

**`tools/patches/v8/apply.sh`** re-applies all six patches idempotently and is
*self-verifying* (each step greps its own marker, dies loud if upstream drifted —
re-grep the anchors here then). It MUST run after every `gclient sync`: Patches 1
& 2 live in `//build`, which sync re-pulls at its DEPS-pinned revision and thus
wipes. Verified byte-identical to the validated edits on a pristine tree.

The six patches: 1–4 are the GN-target skeleton (above). **5 & 6 are the two
appendix-A `platform-linux.cc` chase fixes** — pre-applied so the first
`ninja v8_libbase` clears them: Patch 5 guards the `<sys/prctl.h>` include for
b1nix (no such header; prctl never called), Patch 6 stubs `OS::RemapShared` to
`nullptr` under `__b1nix__` (no mremap; shared-cage path, dead under `--jitless`).
Both confirmed real against the b1nix sysroot and verified on the real source.

The two throwaway stubs the v0.56.12 validation hand-authored (`gclient_args.gni`,
`third_party/icu/config.gni`) are **no longer needed** — a real `gclient sync`
provides both. apply.sh does not create them.

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
| `#include <sys/prctl.h>` (:15) | **header missing** — but `prctl()` is **not actually called** in the file (confirmed: only the include) | **DONE — apply.sh Patch 5** guards the include for `__b1nix__`. |
| `mremap(…, MREMAP_FIXED\|MREMAP_MAYMOVE)` (:81) | **no `mremap` syscall/libc** | **DONE — apply.sh Patch 6** stubs `OS::RemapShared`→`nullptr` under `__b1nix__` (shared-cage only, dead under `--jitless`; see below). Add `SYS_MREMAP` later only if the shared cage is enabled. |
| `<sys/sysmacros.h>` + `makedev()` (:326) | **present** (`userspace/include/sys/sysmacros.h`) | none ✅ |
| `<sys/mman.h>` mmap/munmap/madvise | present; `madvise`/`MAP_NORESERVE` landed v0.56.6 | none ✅ |
| `/proc/self/maps` parse (:276, `SignalSafeMapsParser`) | procfs has `maps` (`kernel/fs/procfs.c`) **but emitted only 4 columns** — V8's parser reads `offset major:minor inode` strictly and aborts on the missing `dev`/`inode` | **FIXED v0.56.9** — `r_pid_maps` now emits Linux-format `start-end perms offset 00:00 inode path` (real vfs inode for file maps). Also fixes pmap/lsof/glibc-style backtrace parsers. |

Net new b1nix gaps surfaced for the platform layer: **one** (`mremap`). Both
`prctl`-include and the maps-format issue are now trivial/done.

**Raw `__linux__` recon (whole `src/`):** exactly **one** site —
`src/debug/wasm/gdb-server/transport.cc:342` — and **none in `src/base`**, so
`v8_libbase` is clean (b1nix gets `V8_OS_LINUX` via Patch 3 but not `__linux__`).
That lone site is the WASM gdb-server (built only with wasm+debug, not in the
jitless `d8` path; b1nix harmlessly takes the non-`__linux__` branch). Defer
unless it actually compiles. The runtime
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
