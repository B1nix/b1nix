# M122: corruption and SMP defects

What was found, what was fixed, and what the evidence for each is. Everything
below is on x86_64 and aarch64 full smoke (1464/0 and 1411/0) unless a line
says otherwise.

## Proof harness: fsverify

`tools/run/soak/fsverify.sh` boots the `fsverify` soak workload with a fresh
ext4 disk and a fresh btrfs disk. Writers on every CPU create, rewrite in place,
truncate, rename and delete files; the guest prints each survivor's sha256.
The verdict is taken on the host with tools that are not this kernel:
`e2fsck -fn`, `btrfs check --readonly --check-data-csum`, and extraction with
`debugfs rdump` / `btrfs restore`, whose content must match the manifest
exactly.

Result after the fixes: clean at 1, 2 and 4 CPUs with a 1 GiB guest and up to
1080 files per filesystem, and at 6 CPUs and scale 300 in 14/14 runs of the
closing campaign (after two of thirty-one failed before the write-back fix
below). Alongside it, 10/10 `soak all` runs at 6 CPUs and 26/26 of the `gfx`
workload that exposed the double reap.

On the passed-through UHD 630 the same fix is checked on the frame the display
engine reads: boot KDE with `b1nix.drm-framedump-key b1nix.drm-framedump-rgb
b1nix.drm-framedump-step=1`, press F12, and run
`python3 tools/run/fd-image.py smoke_run/i915-passthrough.log 0 out.png 603 287
1298 782`. Before the page-cache fix that frame held four page-aligned 4 KiB
black runs inside the terminal window; after it, none.

## Defects found and fixed

- **PMM metadata under the AP trampoline.** `find_early_mem` placed the
  allocation, buddy and page-table-claim bitmaps at physical 0. The SMP
  trampoline is copied to 0x8000: on a 384 MiB guest its bytes landed in the
  page-table claim bitmap (frames at 256 MiB with no history reported as "live
  page table"), on a 2 GiB+ guest in the allocation bitmap itself (frames at
  1 GiB marked free while in use). Early metadata now starts at 1 MiB.
  Reproduced in 5 s by fsverify before, 0 after.
- **Block cache: unclaimed single-block writeback.** `blk_flush_buffer` wrote a
  slot without the BUSY claim and cleared DIRTY afterwards, discarding a write
  that landed during the DMA — the "new header, older tail" of a multi-block
  structure. `blk_cache_invalidate` zeroed slots with I/O in flight, and
  `blk_flush_matching` returned while other CPUs still had matching blocks in
  flight, so a filesystem barrier could precede its data.
- **Truncate lost the straddling page.** The linuxkpi
  `truncate_inode_pages_range` dropped the folio containing the new end of
  file instead of zeroing its tail; with ext4 delayed allocation the kept bytes
  existed only there and came back as zeros.
- **ext4 directories lost half their entries.** The 64-bit htree cookie sets
  bit 62, which the VFS directory cursor uses as its phase bit. Imported
  filesystems now get 32-bit cookies (`FMODE_32BITHASH`), and a cookie with the
  bit set is refused instead of truncating the listing.
- **Page-cache insert race.** Two threads faulting one shared file page each
  mapped their own frame; the loser's writes were invisible to other mappers
  (KWin showed 4 KiB zero runs in client windows). The loser now adopts the
  cached frame.
- **No reclaim for imported filesystems.** ext4/btrfs mappings were never
  shrunk; kswapd now drops clean, unreferenced folios after the kernel page
  cache.
- **Double free of an exec staging buffer** (`kheap: double free of a large
  block`, `user_image_free` <- `scheduler_waitpid`). An exec installs its image
  in the task and only then maps the segments and frees their staging buffers;
  a reaper that finds the task dead in that window runs `user_image_free` over
  the same slots, and both freed the same block. Each site now takes the
  pointer out of the slot with an atomic exchange and frees only what it took.
  Reproduced about one init-lane run in ten before the fix (`SKIP_BUILD=1
  SMOKE_INSTANCES=init sh tests/smoke.sh x86_64` in a loop, after `make
  iso-init`); 0 in 60 runs after it.
- **Task claim.** A CPU now claims a READY task by CAS on its kernel-stack
  lease. The lease is published in exactly two places: by
  `arch_context_switch`, after the outgoing context has been saved and the
  stack pointer swapped, and by `sched_handoff_recover`, which now refuses a
  task that is mid-switch (`g_task_switching_out`) or is some CPU's current
  task. Since a claim takes the lease with a CAS, at most one CPU can hold it,
  and therefore at most one CPU can load a task's saved context -- whatever a
  waker did to its state meanwhile. 30/30 `SMP=6` vm-none/vm/spawn soak runs
  clean.

- **One task reaped twice** (`pmm_free_frame: double-free of physical frame`,
  `unmap_page_from_pml4` <- `user_address_space_cleanup` <- `scheduler_waitpid`,
  about one `gfx` soak run in eight). The fatal-signal branches of
  `scheduler_deliver_pending_signals` mark the task DEAD and return, with the
  SIGKILL still pending; the dying task runs on to its final switch-out, and
  every further pass through delivery ran the death again and stored TASK_DEAD
  over the TASK_REAPING its parent's `waitpid` had claimed. The reaper then
  claimed the same task, and both freed its address space and kernel stack.
  Found by recording, per CPU, the root being torn down with each freed frame:
  both frees named one root and one task. A dead task now delivers nothing.
  aarch64 had relied on the repeat: its return-to-user path exited a
  default-action kill with `128 + sig`, which waitpid reads as a normal exit,
  and the second pass happened to overwrite it; it now reports the signal.
  0/26 `gfx` runs after, 3/20 before. This is also the most likely source of
  the kernel stack overwritten under a `#GP` below: a stack freed while its
  task was still being torn down.
- **Starvation under pinned CPU-bound threads.** One global stride virtual time
  cannot keep tasks pinned to different CPUs on one scale: a woken task landed
  170 million passes behind soak's pinned `cpustress` burners and never ran
  (`vm` then `cpu` hung every time). A candidate's pass is now held within 200
  nice-0 turns of the lowest one its CPU can run.

## Watched, not reproduced

- **The btrfs checksum miss** (`mirror 1 bytenr ...` in two of thirty-one heavy
  fsverify runs): the write-back anchor fix above covers the observed shape;
  0/24 heavy runs since.
- **A `#GP` with `rip` a small integer**, once: the return address on a kernel
  stack had been overwritten. The double reap above freed a kernel stack while
  its task was still being torn down, which produces exactly that; it has not
  reappeared. The switch path still refuses to run a task that is already some
  CPU's current task, so a two-CPUs-one-stack case would panic by name.
- **Only with the host overcommitted about 3x** (a full smoke run and an
  `all` soak started together on 8 host threads): a spinlock lockup on the
  TLB-shootdown lock in the spawn phase (2 of 9 such runs, 0 of 6 since) and
  once a `BCACHE-TEST` writer left BLOCKED on a virtio-blk completion. Neither
  reproduces on an unloaded host or under synthetic host CPU load. A lockup on
  the shootdown lock now names the round in flight: holder CPU, operation,
  pending count, and each CPU that has not acknowledged with the task it runs.
- **Memory pressure.** Heap growth, page-table allocation, `fork` and large
  allocations report ENOMEM, but fsverify at 384 MiB with scale 200 still
  degrades rather than finishing. Belongs to M127.
- **i915 `uncore->lock` lockup during probe**: 0/30 boots before and after; kept
  in [i915-gen9-passthrough.md](i915-gen9-passthrough.md).
