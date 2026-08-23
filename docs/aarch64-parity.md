# AArch64 parity with x86_64

**This is not a milestone.** There is deliberately no "MNN: ARM port" entry in
the roadmap. AArch64 is a second target of the *same* kernel, so every gap below
belongs to the milestone that owns the mechanism — a missing fault-handler case
is a memory-management item, an unbuilt port is a ports item. This document is
the inventory of what differs between the two targets, why, and the procedure
for closing a gap.

Branch: `aarch64-reviving` — merged with `main`, 0 behind.
Suite (`sh tests/smoke.sh aarch64`): **1143 passed / 0 failed / 0 blocked /
35 skipped** at v0.106.198, twice in a row. (It was 769/195/41 at v0.106.34 and 1027/35/0 at
v0.106.62; see §1.) Every remaining skip is named, with its reason, in §2.5 —
none of them is a check quietly dropped.

x86_64 is unaffected except where noted under
[Shared code](#26-shared-code-touched-by-the-port); it was in fact broken by
this branch until `74bacff`, which is why that section exists.

---

## 1. How the port got here

Condensed history, because the root causes explain the shape of the remaining
gaps.

| What | Root cause found | Commit |
|---|---|---|
| Port revived, boots musl userspace on QEMU virt | — | `2be21cf` |
| Linux-ABI syscall numbers, `clone`/`vfork`, DTB memory | per-arch number tables were x86_64's | `e328587` |
| Per-arch `struct stat`, real FP/SIMD context switching | V registers were never saved | `14e2a82` |
| Signal number clobbered in `x0` | syscall epilogue overwrote it | `ed3ae7d` |
| SHARED pages stopped being shared across fork | fork lost `SW_SHARED` | `7c9b0cc` |
| `CLONE_SETTLS` on the new thread | TLS installed on the wrong task | `f118dc4` |
| PSCI poweroff/reset | no firmware call at all | `0e099e7` |
| **Kernel heap aliased identity-mapped RAM** | `KHEAP_START` sat inside the boot identity map, so heap objects and pmm frames shared addresses | `047ede7` |
| virtio-net over the mmio transport | only the PCI transport existed | `fe5ff29` |
| ARP never answered | `aligned(4)` on member types beat `packed`; wire structs were 2 bytes long (**both arches**) | `0c8f556` |
| Real time of day (PL031 RTC) | CMOS-only driver | `f675a12` |
| `waitid(2)`, `seccomp(2)` numbers | x86_64 numbers in the aarch64 table | `8c09553`, `faa21f1` |
| b1nix's `SYS_*` aliases shadowed musl's numbers | unconditional `#define`s | `8f3a53b` |
| Kernel-heap bounds hardening, kernel-stack guard slot | stack top was the exclusive end of its allocation | `c48f761` |
| **x86_64 stopped booting entirely** | the kheap growth guard compared `vmm_virt_to_phys()` to the address, which is how *aarch64* spells "not mapped"; x86_64 spells it 0, so it panicked at the first heap growth of boot | `74bacff` |
| Copy-on-write fork, per-page TLB invalidation, swap-in, reference bit | the pager had the flags but none of the mechanisms | `b561a39` |
| `msync(2)` never wrote anything back | `paging_test_and_clear_dirty` returned 0 unconditionally, so every page looked clean | `b561a39` |
| 122 smoke checks | their ports were already cross-built here; the arch exclusion list was stale | `e73dcbb` |
| Every EL1 fault dump described the *second* fault | `sync_el1` built its entry frame on the interrupted SP_EL1, so a bad pointer made the frame build fault and the nested exception overwrote ESR/ELR/FAR. A dedicated entry stack shows the first fault — and says SP_EL1 was the task's own stack top all along | `78748fc` |
| SP_EL1 drifted on the way out to EL0 | the reset was applied entering EL1 only; three paths `eret` without a matching SAVE_REGS | `ed00cdb` |
| `select(2)` could report readiness the fd_sets cannot express | the Linux-ABI `pselect6` shim counted any revent as ready but wrote back only POLLIN/OUT/ERR/HUP, so a POLLNVAL fd returned "1 ready" with every set empty | `27d5cb4` |
| Every kernel panic printed bare addresses (**both arches**) | `panic_backtrace` symbolicated through a hand-registered table nothing registers into, ignoring the kallsyms blob | `e73dcbb` |
| The suite swung between 1022 and 640 passed on the same tree | not a regression: the `sys` lane wedged in `m32_smoke`'s TLS teardown. It `kill`s its single-shot server with **SIGTERM**, but the test runner opens with `trap '' … TERM …` (`tools/ports/00-smoke.start`) and **SIG_IGN is inherited across execve as well as fork** — so the kill was a no-op and the `waitpid` behind it never returned. Everything sequenced after `m32_smoke` (~380 checks) then never ran. SIGKILL at the three cleanup sites | v0.106.62 |
| `std::filesystem::remove_all` deleted nothing and reported ENOTEMPTY | **the O_* flag numbers are not shared across Linux architectures.** x86_64 keeps its historical numbering; aarch64 uses asm-generic, where `O_DIRECTORY` is 040000 (x86_64: 0200000), `O_NOFOLLOW` 0100000, `O_DIRECT` 0200000, `O_LARGEFILE` 0400000. The translation used x86_64's values, so on this arch `O_DIRECTORY` was read as O_NONBLOCK and `O_DIRECT` as O_DIRECTORY — opening a *file* with O_DIRECTORY succeeded. libc++ builds `remove_all` on that probe (openat(O_DIRECTORY), expect ENOTDIR for a file), so it never deleted anything and surfaced the final rmdir's ENOTEMPTY | v0.106.64 |
| `M37-E1000 rx-arp` never saw a reply (6 checks) | not the driver: this lane deliberately gives the e1000 its own SLIRP (`10.0.3.0/24`) so two user-nets do not both answer for the same gateway, while the in-kernel self-test was hardcoded to ARP for `10.0.2.2`. The request went to a gateway that is not on this NIC's segment. Subnet is now the `b1nix.e1000-subnet` cmdline parameter (default 2 = the x86_64 lane) | v0.106.65 |
| `NATIVE-SMOKE` printed 17 NUL bytes instead of its message | **ELF segments were demand-paged into a fault handler that cannot read files.** `user_load_elf64` lazily maps read-only segments (`vmm_set_lazy` + a VMA carrying node/offset), and this arch's handler treats `VMM_LAZY` as "the page is zero" without ever consulting the VMA — so the process got zeroes where its own `.rodata` should be. The `write` syscall worked perfectly and emitted 17 NULs; the data simply did not exist. Demand paging is now off here (`demand_page = 0`), which costs resident memory and is correct. Re-enable it the day the fault handler reads — it is the same missing mechanism as the `ld.so` bug in §1 | v0.106.69 |
| Every synthetic directory listed only ONE entry (**both arches**) | the `getdents` shim ended the walk on `after <= before`, i.e. it required the directory cursor to grow — contradicting its own comment that the cookie is opaque. The in-memory readdir's cookie genuinely **descends**: children are inserted at the head of the sibling list, `dir_seq` falls along it, and each cookie is the last emitted seq. So `/sys/block` listed one disk (the newest, `loop7`) while `sda`/`sdb`/`sdc`/`vda`/`loop0` were all openable by name. Now only `after == before` (no movement) stops the walk | v0.106.68 |
| `M98` memory typing / cache maintenance (3) had no implementation here | the API existed only in `kernel/arch/x86_64/memtype.c`. Both jobs exist on ARM, spelled differently: the memory type is an MAIR slot picked by the descriptor's AttrIndx (Attr2 = Normal-NC, this arch's write-combining) rather than a PAT MSR, and eviction is `DC CIVAC` by address plus a set/way walk over every level for the whole-hierarchy case. New `kernel/arch/aarch64/memtype.c`; the old `cache_flush_range` in `arch.c` (hardcoded 64-byte line) was folded into it and now reads `CTR_EL0.DminLine`. Only the PAT-MSR readback has no counterpart and is skipped by name | v0.106.66 |
| `M98` PCI checks (8) were never run here | `pci_selftest()` sat behind `#if defined(__x86_64__)` because "this port has no PCI host bridge" — stale since ECAM landed, which is how e1000/xHCI/NVMe are found here. Only `memtype_selftest()` (PAT) is genuinely x86; the PCI half now runs on both arches and passes, MSI/MSI-X included | v0.106.65 |
| `struct epoll_event` was packed everywhere (**both arches' header, one arch's ABI**) | Linux packs it on x86_64 alone (`EPOLL_PACKED`), and musl matches with `#ifdef __x86_64__`. The kernel put `data` at offset 4 where aarch64 userspace reads offset 8, so every `epoll_wait` handed back half an events mask as the caller's pointer | v0.106.70 |
| `PTRACE_GETREGSET(NT_PRFPREG)` handed back x86's 512-byte FXSAVE area | this arch's set is `user_fpsimd_state` (528: 32 V registers + FPSR + FPCR). `struct user_fpregs_struct` is per-arch now, and the task save area carries that layout — so **FPSR/FPCR are saved across context switches for the first time**, having been skipped by `fpu.S` entirely | v0.106.70 |
| `NT_ARM_TLS` returned `-EINVAL` | the regset switch knew only x86's three; the thread pointer is `task_tls_base()` | v0.106.70 |
| An `SA_SIGINFO` handler's third argument was always null | the ucontext was never built here ("nothing reads `uc_mcontext` on this arch yet"). A crash reporter does — Crashpad read the crashing thread's registers from address 0. A real Linux `ucontext_t` is built now, `fpsimd_context` record and all | v0.106.70 |
| Delivering a signal panicked the kernel whenever the handler had a fresh `sigaltstack` (crashpad) | `vmm_handle_page_fault` refused **every** kernel-mode fault: `if (!(error_code & 4)) return -1`. x86_64 services a not-present fault anywhere in the user range regardless of PF_USER, and it must — the kernel writes into user memory on the task's own behalf. `arch_build_signal_frame`'s first store to an untouched alt-stack page therefore took a fatal EL1 abort (`ESR 0x96000046`, FAR in the mmap'd stack). Now an EL1 fault below `USER_SPACE_LIMIT` goes through `fault_anon_user_page` like any other, which still demands a VMA, so a wild kernel pointer is refused rather than backed | v0.106.70 |
| Nothing ever read the serial port's receive FIFO | `serial_tty_tick()` — the drain for whoever holds `/dev/ttySn` open — was called only from the x86_64 timer vector. The console here was output-only: a getty on the serial line printed its prompt and never saw a keystroke. Called from `aarch64_irq_handler`'s timer branch now, the same place and order x86_64 uses | v0.106.70 |
| `TCR_EL1.IPS` was a constant (40-bit output) | `ID_AA64MMFR0_EL1.PARange` uses exactly the IPS encoding, so boot.S reads it and clamps to 5 (48-bit; 6 means FEAT_LPA and a different descriptor format). The hardcode asked a narrower CPU for an output size it does not implement and capped a wider one at 1 TiB | v0.106.70 |
| A signal handler never ran in a task making no syscalls | `aarch64_irq_handler` took no register frame and never called `arch_check_and_deliver_signals`; x86_64 delivers from its timer vector. Default-action *termination* hid it, because the scheduler does that itself without a user frame | v0.106.62 |

| PCI interrupts were never delivered — AHCI, NVMe, virtio-blk, virtio-gpu and e1000 all polled | config space 0x3C (Interrupt Line) is **firmware-filled**, and no firmware runs before this kernel on arm64, so every driver registered its handler on a line nothing arrives at. The routing lives in the host bridge's device-tree `interrupt-map`, captured now in `bootinfo.c` and decoded by `pci_intx_line()`. Entry width is worked out from the table rather than assumed: virt's GIC declares two address cells, so an entry is 10 cells, and reading it as 8 put every device on GIC ID 32 | v0.106.196 |
| `M14-BLK` durability (7 checks) was x86-only | not an arch gap: `blk_durability_selftest` took the **first** virtio disk as its scratch, which on this arch is the root filesystem — it would have written its pattern a megabyte into a live root. It now picks the first virtio disk `vfs_device_is_mounted()` says nobody has mounted, and the mmio virtio-blk driver negotiates and implements `VIRTIO_BLK_T_FLUSH` (it had been declining every optional feature, so `fsync` reached the host's page cache and stopped) | v0.106.196 |
| Root mounted by hardcoded name | `vfs_mount("vda", …)` held only while one virtio disk existed. Attaching a scratch disk gave the newcomer the lower mmio slot, so the root became `vdb` and the machine booted the initramfs with a good rootfs one letter away. `mount_first_virtio_root()` tries each in turn | v0.106.196 |
| The image had no packaged ports at all on a clean tree | `install-ports` took a much shorter path here on the belief that Alpine ships no aarch64 packages. It does — the lock has aarch64 hashes for zsh, sway, foot, cage, mesa, xwayland, curl, dropbear, samurai and bmake. The two arches share one recipe now; the checks covering those programs had been passing on whatever an earlier build left in the rootfs | v0.106.196 |
| `M52-GFX gpu-irq` failed silently | two things: the GPU had no interrupt (above), and **no check read the marker** — the self-test printed `fail` into the log and the suite reported a clean run. The check exists now, and the count assertion distinguishes MSI-X (one message per completion, exact) from a level-triggered line (coalescing is legal: the GIC delivered 6 for 8 commands) | v0.106.196 |
| Only one CPU ever ran | no secondary bring-up at all. PSCI CPU_ON does it: `_ap_start` repeats the boot processor's MMU prologue against the **live** kernel tables (by then the heap and the MMIO window are outside the boot identity map), and `aarch64_ap_main` runs the work-stealing loop x86_64 runs before its userspace phase. Three separate defects had to be cleared first — the device-tree walk cleared its "inside /cpus" flag on leaving one of `cpu-map`'s children, so four listed CPUs read as one; the per-CPU index was derived from a counter it also advanced, numbering them 1, 3, 6; and `ap_worker_trampoline` was an empty stub here, so a stolen worker's entry never ran. Per-CPU data is reached through MPIDR_EL1, not a thread pointer: TPIDR_EL1 is already the vectors' scratch register | v0.106.198 |
| Merge with `main` brought x86-only code into shared files | `rep movsb`/`rep stosb` in `string.c`, a TSC clock, `paging_move_range`/`paging_reload_cr3`/`paging_user_pte`/`vmm_query_leaf_pte`, `frame->rip`, `context.rsp`, a fifth `netdev.transmit` argument and bare `pause` in four drivers. All ported rather than re-gated; the monotonic clock is CNTVCT_EL0/CNTFRQ_EL0, which needs no invariance check | v0.106.195 |

Two findings did **not** turn into commits and are the most valuable things to
carry forward:

- **The `ld.so` failure is solved analytically.** `sys_mmap` marks file-backed
  pages `VMM_LAZY`, and this arch's fault handler treats that marker as "the
  page is zero" — it never looks at `vma->node`/`vma->offset`. So every
  `mmap` of a file read back as zeroes, `ld.so` mapped each shared library as a
  blank region, parsed an empty dynamic section and died dereferencing
  `dso->hashtab == NULL` in `find_sym`. Implementing the read makes
  `M32B-SSH: ok dropbearkey` pass. It is not on this branch because every
  variant that actually reads file content exposes the fault below.

  Implemented on branch `aarch64-file-mmap-wip` (`9ec32c1`): an eager fill at
  `mmap` time — the vectors enter with IRQs masked, so the fault handler cannot
  do the blocking read x86_64's does — mapping page-cache frames copy-on-write
  rather than copying them.
  Implemented on branch `aarch64-file-mmap-wip` (commit `9931154`) as an eager
  read at `mmap` time rather than in the fault handler, since the vectors enter
  with IRQs masked. It works — `/bin/dropbearkey` loads its libraries and
  generates a host key — and it still drops the suite to ~350 through the fault
  below, so it is kept off `aarch64-reviving`.
- **An open fault at exactly `heap.end`.** Scattered lanes die with an EL1 data
  abort whose FAR equals `heap.end`. Instrumentation ruled out an unmap
  (a trap over `[KHEAP_START, KLARGE_START)` never fired), a heap shrink
  (page-return is compiled out here), and the allocator handing out a block past
  the mapped end (both new bounds checks in `c48f761` are silent). The decisive
  dump: `sp` sits just below `heap.end` but *above* `heap.current` — in mapped
  but never-allocated tail — while `current_task->kernel_stack_ptr` points
  megabytes away. The CPU is running on a stack that is not the current task's
  and that the allocator never handed out. This predates the mmap work; it is
  why heap page-return is disabled on this arch. **Prime suspects:** where
  `g_aarch64_kernel_stack_top` / `EL0_KSTACK_RESET` can disagree with
  `current_task`, and the fork child's
  `kernel_stack_ptr = parent->kernel_stack_ptr + stack_offset` relocation.

  **Ruled out since**, each with a trap that ran the whole suite and never
  fired — do not re-derive these:

  | Hypothesis | How it was tested |
  |---|---|
  | A bad kernel-stack top is published | assert in `arch_set_kernel_stack` (now permanent) that the value lies inside allocated heap |
  | A task is resumed with a bad `context.sp` | the same bound check on `new_task->context.sp` at every switch-in |
  | A live heap page gets unmapped | trap in `paging_unmap_page_from_space` for any VA in `[heap.base, heap.current)` |
  | Kernel-stack overflow | canary at the base of every kernel stack, checked on each switch (now permanent) — intact |
  | `heap.current` moves backwards | monotonicity assert on every read |
  | The stack is a real heap block | `kheap_describe(sp)` says `not-in-general-heap`, while the task's own stack is a `LIVE` block megabytes away |
  | The free list handed out a block running past `heap.current` into the tail | assert on every reuse that `block + header + size <= heap.current` — the bound has to be `heap.current`, not `heap.end`, or the mapped tail hides it |
  | A frame is handed out while still mapped | the pmm's own refcount-0 and double-free guards report only module frames (`0x41003000`+), never a user page |

  So the running `sp` is neither published, nor restored from a saved context,
  nor a block the allocator ever handed out, and the memory under it was never
  unmapped.

  **How far the suite gets scales with how many physical frames the mmap path
  consumes** — 184 (a private frame per page, no page cache), 293 (private
  copies of cached pages), 351 (the first eager version), 428 (mapping cached
  frames copy-on-write). That looks like memory pressure, but giving the lane
  2 GiB instead of 1 makes it *worse* (232), which rules simple scarcity out.
  What varies across all five is which physical frames get reused where. That
  fits a stale user mapping writing into a frame that has since been handed to
  the kernel heap — so the next thing to instrument is frame ownership
  (who still maps a frame at the moment it is handed out again), not the stack.

  **Two measurement lessons.** Runs on this host are noisy — the same tree has
  produced 222 / 361 / 418 / 428 — so a single run is not evidence. Two changes
  were called regressions on one run each (732/0/273 and 594/0/411) and both
  re-ran clean at the baseline; always confirm a suspected regression with a
  second run. And a `#` sentinel that only fires after N events (a spin
  detector, a periodic print) may simply never be reached in a run that dies
  early — check that the probe fired at all before drawing a conclusion from
  its silence.

  **A partial fix that is not safe yet.** SP_EL1 is made absolute on the way
  *into* EL1 (`EL0_KSTACK_RESET`) but not on the way out, so every path that
  `eret`s without a matching `SAVE_REGS` — `aarch64_eret_frame`,
  `x86_user_jump`, `aarch64_user_thread_jump` — leaves it wherever it stopped.
  Doing the same reset before each `eret` to EL0 (branch
  `aarch64-file-mmap-wip`, `b465421`) is the principled version of that
  invariant, and it measurably helps: the `sys` lane stops dying and the boot
  lane's panics halve. But on the plain branch it **hangs** the `posix` and
  `init` lanes (stall, not panic), taking the suite to 732/0/273 — so it is not
  on `aarch64-reviving`. Find out which of those three exit paths must not take
  the task's stack top before reusing this.


---

## 2. Gap inventory

### 2.1 Memory management — the big one

`kernel/arch/aarch64/paging.c` is 1022 lines; `kernel/arch/x86_64/paging.c` is
~2554. The difference is not style, it is missing mechanisms.

| Mechanism | x86_64 | aarch64 | Consequence today |
|---|---|---|---|
| File-backed demand paging | page cache + `read_cb`, filled outside the VMM lock | **absent** | every `mmap` of a file reads as zeroes → the `ld.so` bug. ELF loading no longer relies on it (v0.106.69 forces eager segment loading here); a lazily mapped segment silently became a run of NULs |
| Copy-on-write | `VMM_COW` set on fork, resolved in the fault handler | **done** (`b561a39`) | — |
| Swap-in on fault | `VMM_SWAPPED` case | **done** (`b561a39`) — mark, fault back in, and walk the space for fork/execve | — |
| Reference / dirty bits | hardware A/D bits | **done** (`b561a39`) — AF-fault reference bit; dirty is conservative, see the source | msync writes back a little more than it must |
| TLB invalidation | per-page, plus cross-CPU shootdown | **done** (`b561a39`) — `tlbi vaae1is`, full flush kept only for address-space switch and the fork COW pass | — |
| Locking in the fault path | `vmm_lock`, with file I/O deliberately outside it | none — and not needed until SMP, since `g_max_cpus = 1` here | revisit with PSCI SMP |
| Break-before-make on block split | n/a | **deliberately violated** (`ensure_child`) | first split is of the 1 GB block holding running code |
| Heap tail page-return | enabled | **disabled** as a workaround | heap high-water only grows |
| `vmm_map_mmio` builds a real mapping | yes | **done** (v0.106.70) — a 32 GiB bump-allocated window at 416 GiB | a caller now gets the memory type it asked for: Device-nGnRE for `ioremap`, Normal-NC for `ioremap_wc`. See below |
| `mremap` leaf-entry move | `paging_move_range`, a block at a time under the write lock | **done** (v0.106.196) — page at a time; there is no fault-path lock here to amortise | — |
| Address-space teardown | frees the user half | skips `L0[0]` by design (shared kernel half) | correct, but the split is easy to get wrong |

**Rule that must not be broken on this arch:** every new kernel virtual address
must stay out of `[0x40000000, DIRECT_MAP_MAX)` (the boot identity map) and
under 512 GiB (inside `L0[0]`, the half every process shares by pointer). Heap
is at 64 GiB, the large-allocation arena at 128 GiB, modules at `0x41000000`.

#### The MMIO window: reverted once, and what made the second attempt stick

`vmm_map_mmio` returned the identity address for most of the port's life: every
driver was really using the boot map, so a caller asking for a type that map
does not carry (`ioremap_wc` on a RAM frame) silently got Normal WB, and `M99
ioremap` failed on both halves of its check — the mapping was the direct-map
alias, and the WC attribute was absent.

A real window was built and reverted inside v0.106.63. The probes then showed
the mappings were correct when made (`0x…403` Device, `0x…70b` Normal-NC) and
genuinely shared (`l0[0]` read the same in kernel and process context), but that
they later **stopped existing**: e1000 read its MAC at probe and then took a
level-3 translation fault on the same registers from process context with
`paging_leaf_pte(FAR)` = 0.

Rebuilt in v0.106.70 at 416 GiB (32 GiB, bump-allocated, never reclaimed —
which is what `iounmap` in `kernel/lkpi/lkpi_core.c` already promises) and the
disappearance did not come back: the whole suite runs green with every AHCI,
NVMe, e1000, xHCI, virtio-gpu and ECAM BAR going through it. Several
address-space defects were fixed between the two attempts, so the fault the
first one hit is most likely one of those rather than anything about the window
itself. If it ever returns, the lead recorded then still stands: chase the
owner of the tables, not the mapping.

`encode_leaf` learned Device memory at the same time: `VMM_PCD|VMM_PWT` (x86's
UC) now selects MAIR slot 0. Only `vmm_map_mmio` passes that pair, so nothing
that lives on the identity map changed type under it.

#### M80 `crash-capture`: a blocking syscall slept through its own SIGSTOP — FIXED

`PTRACE_ATTACH` returned 0 and the stop never arrived, which also cost the
three checks sequenced behind it in the same function (`ptrace-getregset`,
`ptrace-fpregs`, `process-vm-rw`).

Attach deliberately only leaves SIGSTOP *pending*, so the tracee stops "at its
next return to ring 3, where its register frame is complete"
(`kernel/sched/ptrace.c`). The crasher is parked in `pause()` — which is
`ppoll()` here, this arch having no `SYS_pause` — and `sys_poll`'s sleep asked
`scheduler_signal_pending()`, which reports a signal **only when it has a user
handler**. SIGSTOP can never have one, so the sleep never woke and the return
to userspace never happened.

Fixed with `scheduler_signal_pending_or_stops()`, used by the poll/select sleep
alone (the narrow predicate has a dozen other callers across sockets, unix, tty
and input, and widening it wholesale is not safe).

**It only covers the STOP group, and that is deliberate.** A first version also
reported every fatal default action and **wedged the posix lane**: the syscall
returned `-ERESTARTSYS`, was restarted, saw the same pending signal and span.
A fatal default needs no return to userspace — the scheduler kills such a task
itself at the next context switch (`scheduler_deliver_pending_signals`), which
is the same asymmetry that made the first version of `signal-compute-loop`
pass without any IRQ delivery at all. Stopping is the case that genuinely
requires the tracee to reach userspace.

### 2.2 Arch files that exist only for x86_64

| File | What it provides | aarch64 status |
|---|---|---|
| `ap_trampoline.S`, `lapic.c`, `tlb.c` | secondary-CPU bring-up, cross-CPU shootdown | replaced, not missing: `kernel/arch/aarch64/smp.c` starts the secondaries over PSCI, and TLB maintenance needs no IPI at all — `tlbi …is` is broadcast across the inner-shareable domain by the hardware |
| `fb_console.c`, `font8x8.h` | framebuffer text console | **absent** — `fb_console_init()` is `#ifndef __aarch64__` in `kernel/main.c`. `/dev/fb0` itself works (M47 passes); what is missing is the kernel's own text console on it |
| `syscall_entry.S`, `user_jump.S` | syscall entry, first entry to ring 3 | folded into `isr.S` (`svc`, `eret`) — fine |
| `io.c`, `rtc.c` | port I/O, CMOS clock | not applicable — PL031 in `kernel/dev/rtc_dev.c` |
| `gdbstub.c`, `coredump.c`, `memtype.c` | remote debugging, ET_CORE dumps, memory typing | **ported** ✅ |

### 2.3 Subsystems still stubbed in `kernel/arch/aarch64/arch.c`

PCI is no longer among them: `kernel/dev/pci.c` speaks the ECAM window QEMU
virt exposes, and `ahci.c`, `nvme.c`, `usb_xhci.c`, `e1000.c` and the virtio-gpu
all come up through it, interrupts included (§1). What is still stubbed:

- **Userspace on a secondary.** The CPUs come up and run stealable kernel
  workers; what they do not run is a process. That needs per-CPU exception
  state (SP_EL1 handling is per-task today through one global) and a review of
  every kernel path this port could leave unlocked while only one CPU existed
  — the fault handler in particular (§2.1).
- **The virtio PCI transport** (`virtio.c`, `virtio_blk.c`, `virtio_net.c`) —
  this kernel's version is the *legacy* port-I/O one, which has no meaning
  here. Block and network arrive over virtio-mmio instead; the mmio drivers are
  full peers (flush included).
- **IOMMU** — no SMMUv3 driver, and QEMU virt offers no unit to drive.
- **XSAVE, watchdog, I²C, AC'97/HDA audio, PS/2 mouse, `arch_backtrace`** —
  either hardware this board does not have, or an x86 register file. Audio is
  the one with a real counterpart to write one day: `virtio-snd`.

Panics do print symbol names — the kallsyms blob was always generated here, it
was the shared symbolication that ignored it (`e73dcbb`).

### 2.4 Userspace and ports

The ports tree is gone: both arches take their libraries and programs from the
same Alpine package set (`tools/packages/alpine-ports.map`, pinned by
`tools/packages/alpine.lock`), and the lock carries aarch64 hashes for the
`programs` group — zsh, sway, swaybg, foot, cage, grim, xwayland, seatd, the
mesa runtimes, curl, dropbear, samurai and bmake. `install-ports` is one recipe
for both arches as of v0.106.196; before that the aarch64 branch staged only
userspace and BusyBox, so a **clean** tree produced an image without any of
them while the checks covering them passed on leftovers from an earlier build.

What is still excluded, and why:

| Excluded | Where | Why |
|---|---|---|
| `m64_clang_smoke` | `userspace/Makefile` | the native in-guest Clang is built for x86_64 only |
| `m104_pam_smoke`, `m108_smoke`, `m108shell` | `userspace/Makefile` | linked against libpam; the aarch64 `pam` package is fetched but these have not been re-tried since the Linux-PAM migration |
| `openssl`, `chromium` | `alpine.lock` | no aarch64 hashes recorded yet. Alpine builds both — one `ALPINE_LOCK_UPDATE=1` run adds them |
| b1cc corpus | probed, not gated | the Makefile asks b1cc whether it can *assemble* for this target and builds the corpus when it can. It currently can |

Everything else — the graphics stack, C++, the port smoke tests — builds here
unchanged.

#### Crashpad: five defects between the crash and the minidump — CLOSED

Crashpad itself is no longer in the tree — `main` retired the from-source ports
and its build script went with them — but the five kernel defects it exposed
were all real, all shared code, and all still fixed. Kept here because each one
is the same x86-shaped assumption in a different subsystem, and the next
crash-reporting client will meet them again if they regress.

Upstream Crashpad, unpatched, now attaches to a crashing process here and writes
a real minidump. Getting there took five fixes, each a genuine kernel defect
that the same x86-shaped assumption produced:

1. **Delivering the client's SIGSEGV panicked the kernel.** The EL1-fault
   refusal described in §2.1 — the signal frame is written to a `sigaltstack`
   page that had never been touched.
2. **`struct epoll_event` was packed on every arch.** Linux packs it on x86_64
   alone; musl's `<sys/epoll.h>` says the same with `#ifdef __x86_64__`. The
   kernel therefore returned `data` at offset 4 where aarch64 userspace reads it
   at offset 8, and Crashpad's exception server dereferences `data.ptr` straight
   out of that array — its handler died of SIGSEGV on a garbage pointer the
   moment a client connected.
3. **`PTRACE_GETREGSET(NT_PRFPREG)` returned x86's 512-byte FXSAVE area.**
   AArch64's set is `user_fpsimd_state`: 32 V registers plus FPSR and FPCR,
   528 bytes. Crashpad checks the length and refuses ("Unexpected registers size
   512 != 528"), losing the thread snapshot. `struct user_fpregs_struct` is now
   per-arch, and `struct task`'s save area carries that exact layout — which
   also means **FPSR/FPCR are saved across context switches at last**; `fpu.S`
   skipped them entirely, so a task's rounding mode and sticky exception flags
   were whatever the previously scheduled task left behind.
4. **`NT_ARM_TLS` was unimplemented** (`-EINVAL`). Crashpad asks for the thread
   pointer of every thread it snapshots; it is `task_tls_base()`.
5. **The third argument to an `SA_SIGINFO` handler was null.** The comment said
   "nothing in the ported userspace reads `uc_mcontext` on this arch yet" — a
   crash reporter does, and it is the whole point of one. Crashpad recorded the
   null as the crashing thread's context and read the register set from address
   0 (`pread64: I/O error`, "Couldn't read gprs"). `kernel/arch/aarch64/signal.c`
   now builds a real Linux `ucontext_t` — `uc_mcontext` at offset 176, the
   register file, and an `fpsimd_context` record in the reserved tail. Like the
   x86_64 side, the snapshot is informational: `sigreturn` restores from b1nix's
   own frame, so a handler that *edits* `uc_mcontext` is not obeyed yet.

One warning remains in the handler's log and is not a b1nix gap: musl's
`sched_getscheduler` is a stub that returns ENOSYS, so Crashpad records no
thread priorities on any musl system.

### 2.5 Test-harness and hardware gaps

The lane runs QEMU `virt` with the same devices the x86_64 lanes get — AHCI,
NVMe, xHCI, e1000, virtio-gpu, virtio-tablet — reached over the PCIe ECAM
window, and the `smp` instance runs four CPUs. What the board cannot host is a
DMA-remapping unit, an ITS for MSI, and an HDA/AC'97 codec.

Ordinary lanes take one CPU here, and `sys` takes two (it is where the RCU
grace-period check reads its marker from). Giving all six lanes two vCPUs was
measurably flaky on a laptop host — a lane that loses the race for host CPU
looks exactly like a wedged guest — and buys nothing while userspace still runs
on the boot CPU.

The 36 skips are all of that shape, and each says so by name rather than
vanishing from the count:

| Skipped | Reason |
|---|---|
| `M28-BENCH` | wants four CPUs; the ordinary lanes run one or two. The dedicated `smp` lane runs four and does execute it |
| the IOMMU and AMD-Vi lanes (30) | QEMU virt has no q35/intel-iommu/amd-iommu, and this port has no SMMUv3 driver |
| `M98 msi-delivery`, NVMe MSI-X | GICv2 on virt has no ITS, so there is no doorbell to deliver through; NVMe uses its legacy line |
| `M98 pat-msr` | no PAT MSR — memory types come from MAIR_EL1 slots (see `memtype.c`) |
| `M80 ptrace-xstate`, `avx-context` | x86 register files; this arch's vector state is NT_ARM_VFP and M29 covers its save/restore |
| `M38`, `M79` (`no-dsp`) | no audio device on this board |

`M71-ASLR` reports "not randomised" honestly: PIE randomisation is still
x86_64-only in `kernel/user/process.c`.

### 2.6 Shared code touched by the port

30 shared files carry `__aarch64__` conditionals — `mm.h`, `kheap.c`, `pmm.c`,
`module.c`, `module_alloc.c`, `scheduler.c`, `syscall.c`, `linux_abi.c`,
`process.c`, `vfs.c`, `ext2.c`, `initramfs.c`, `net.c`, `rtc_dev.c`, `video.c`,
`virtio_gpu.c`, the lock headers, `ptrace.c`, `rseq.c`, `seccomp.c`, and others.
Two traps live here:

- In this tree `#ifdef __x86_64__ / #else` historically meant "**32-bit**", not
  "aarch64" — the dead 32-bit port was removed in `a671315`, so any surviving
  `#else` branch is now silently the aarch64 branch. Several bugs
  (`m35_smoke`'s ELF check, `ftruncate`/`utime`) were exactly this.
- Changes made "for aarch64" in shared files (`kheap.c`, `scheduler.c`) ship to
  x86_64 too and must be regression-tested there.

---

## 3. Procedure for porting a mechanism

This is the loop that produced every fix above. Follow it per mechanism, not
per file.

1. **Query the knowledge graph first** (`search_graph`, `trace_path`,
   `get_code_snippet`) to find the x86_64 implementation and everyone who calls
   it. Do not fan out `grep`/`Read` across the tree.
2. **Read the x86_64 implementation in full**, and separate it into three
   buckets: genuinely architectural (page-table format, exception entry,
   register frames), incidentally architectural (written against x86 but the
   logic is generic), and shared logic that should not be duplicated at all.
   Bucket two is where the port earns its keep — move that logic into shared
   code rather than writing a second copy.
3. **Check the ABI/number tables** if the mechanism is user-visible:
   `kernel/include/b1nix/syscall.h` ↔ `userspace/include/syscall.h`, and the
   per-arch `LINUX_NR_*` table in `kernel/include/b1nix/linux_abi.h`. A
   surprising share of "the feature is broken" turned out to be a number copied
   from x86_64. **It is not only syscall numbers**: flag values differ too —
   the `O_DIRECTORY`/`O_NOFOLLOW`/`O_DIRECT`/`O_LARGEFILE` group is renumbered
   wholesale on asm-generic architectures (see `linux_open_flags_to_b1nix`).
   When a library "works on x86_64 and silently does nothing here", check the
   constant before the code.
4. **Implement**, respecting this arch's constraints: the kernel VA rules in
   §2.1, no floating point in the kernel, and `-mgeneral-regs-only` — any code
   naming V registers must live in `.S` (that is why `fpu.S` exists).
5. **Register the file** in the top-level `Makefile` (`ARCH_SOURCES` /
   `KERNEL_SOURCES` / `ASM_SOURCES`) — there is no globbing, and a new file that
   is not listed simply never compiles.
6. **Add or unblock a smoke check.** Prefer deleting an entry from the
   `ifneq ($(B1NIX_ARCH),aarch64)` block in `userspace/Makefile` over writing a
   new test: those binaries usually build unchanged, and each one that starts
   passing is a real, measured gain.
7. **Rebuild and run one lane, then the suite.** `make clean && make iso` after
   touching `userspace/libc`, `userspace/include` or `userspace/crt` — the whole
   initramfs is regenerated from those.
8. **Read the log, not just the count.** `grep -c PANIC
   smoke_run/b1nix-smoke-*aarch64.log` shows which lanes died; a pass count can
   stay flat while a lane wedges. Use `grep -a` — the serial log has binary
   bytes.
9. **Regression-test x86_64** if the change landed in a shared file.
10. **Commit per confirmed gain**, bump `B1NIX_VERSION_STR`, and put the
    mechanism's status in the milestone that owns it — not in an ARM-specific
    list.

### Gotchas earned the hard way

- **Never enable interrupts inside the aarch64 fault handler.** The vectors
  enter with IRQs masked; unmasking around a file read took the suite to 36.
  Blocking work belongs in syscall context.
- **A lazy marker means "reserved", not "zero".** See the `ld.so` finding.
- **Do not donate page-cache frames to a process without a matching
  reference.** Address-space teardown frees leaves marked `SW_USER`, and
  `SW_USER` is tied to `VMM_USER`, so a user mapping cannot opt out of being
  freed. Use `VMM_SHARED` + `pmm_ref_frame`, the way the device-mmap path does.
- **`ensure_child` is not break-before-make** by design; read the comment before
  "fixing" it.
- **Module relocations are per-arch.** `kernel/module/module.c` needed
  `R_AARCH64_ABS64/ABS32/PREL32/PREL64/CALL26/JUMP26/ADR_PREL_PG_HI21/
  ADD_ABS_LO12_NC/LDST*` and an `EM_AARCH64` header check.
- **Packed wire structs**: `__attribute__((aligned(N)))` on a *member type*
  overrides `packed` on the struct. This silently made every ARP packet 2 bytes
  short on both arches. Add `_Static_assert`s on size and offsets.
- **`EI_OSABI` must be 3** on every musl-linked binary; `tools/b1nix-musl-cc`
  patches it.
- **Never trust a self-matching waiter**: `pgrep -f "smoke.sh aarch64"` matches
  the wrapper's own command line.
- **A failing check is not automatically a kernel gap.** Two of the biggest
  groups here (`M37 rx-arp`, the `M98` PCI checks) were a self-test hardcoded to
  the *other* lane's subnet and a stale `#if defined(__x86_64__)`. Read what the
  lane actually gives the guest, and re-read the guard's justification, before
  writing driver code.
- **`bootinfo_get_kv` returns 1 on a match, not 0.** Testing it like a
  `strcmp` leaves the default in place and the parameter silently does nothing.
- **Two mappings of one frame with different cacheability need explicit cache
  maintenance here.** ARM calls these mismatched attributes: a store through a
  Normal-NC alias does not invalidate the write-back view, so the WB read must
  be preceded by a `DC CIVAC` over the range. `memtype_selftest` does this
  deliberately — it is the same sequence a GPU buffer depends on.
- **Before adding an arch file, grep for the symbol.** `cache_flush_range`
  already existed as a one-off in `arch.c`; the new `memtype.c` collided with it
  at link time (`ld.lld: duplicate symbol`), which the per-file compile check
  cannot catch.
- **A quiet aarch64 lane inflates FAIL, never BLOCKED.** `kernel/main.c` ends a
  test instance itself after 280 s of console silence and prints
  `B1NIX-TEST: done`, so the harness never sees the lane as wedged and reports
  every check that never ran as a failure. Check how far each lane's log got
  before believing a failure count.
- **SIG_IGN is inherited across execve**, so the runner's `trap '' … TERM …`
  reaches every test binary and its children. A test that cleans up a helper
  must use SIGKILL; SIGTERM is silently discarded inside the test tree.
- **Default-action termination proves nothing about signal delivery.** The
  scheduler kills such a task itself on the next switch
  (`scheduler_deliver_pending_signals`), no user frame needed. Only a *handler*
  exercises the return-to-user delivery path.

---

## 4. Suggested order of work

1. **Userspace on a secondary CPU.** The CPUs are up and stealing kernel work
   (v0.106.198); what is left is the half x86_64 calls its userspace phase.
   This is where fault-path locking (§2.1) stops being optional.
2. **File-backed demand paging** (§2.1). Eager loading works and costs resident
   memory; the fault handler still cannot read a file, which is why ELF
   segments are loaded eagerly here and `mmap` of a file is filled at mmap time.
   The blocker is that this arch's vectors enter with IRQs masked.
3. **PIE randomisation** — `kernel/user/process.c` gates ASLR to x86_64 with a
   note that ungating it broke musl's loader. That was the same lazy-mapping
   bug as (2); re-try it once (2) lands.
4. **The three PAM smoke binaries and the two missing packages** (§2.4). Cheap:
   the pam package already installs here, and `openssl`/`chromium` need one
   lock-update run.
5. **`virtio-snd`**, if audio is wanted on this board — the AC'97/HDA drivers
   have no counterpart to port, but the milestone's markers do.

Not on this list, deliberately: an SMMUv3 driver. It would close the IOMMU
skips, but QEMU virt provides no unit to test it against, so it cannot be
verified here.
