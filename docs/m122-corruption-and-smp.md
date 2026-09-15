# M122: corruption and SMP defects

What was found, what was fixed, and what the evidence for each is. Everything
below is on x86_64 and aarch64 full smoke (1448/0 and 1395/0) unless a line
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
1080 files per filesystem. At 6 CPUs and scale 300 three of thirty-one runs failed --
see "Not closed" -- and 11/12 earlier at 384 MiB (the twelfth was an OOM).

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

## Not closed

- **One data block with a bad checksum, twice** (`btrfs check`:
  `mirror 1 bytenr ... csum ... expected ...`), in two of thirty-one fsverify runs at 6 CPUs
  and scale 300. Every file extracted from that image still matched the
  guest's manifest, so the data block on the disk is the newest version and its
  checksum entry is not: a lost metadata write. The image is kept at
  `smoke_run/final/fsv/f6-smp6/btrfs.img` and `btrfs check --check-data-csum`
  names the block (`mirror 1 bytenr 203378688`).

  A write-back run that drops a dirty block was found by reading the code.
  `bcache_writeback_run` walks back from its anchor to the start of the
  contiguous dirty region and then writes forward for at most `run_max`
  blocks -- so a walk of the full `run_max` leaves the anchor one block past
  the end of the command. The eviction caller cleared the anchor's DIRTY flag
  regardless, and that block's contents were dropped: the next read of it
  returns what the disk held before. Exactly one lost metadata block, under
  memory pressure, on a device whose maximum transfer bounds `run_max` -- the
  shape observed. The walk back now stops one short of `run_max`, and eviction
  refuses to recycle a block a landed run did not cover.

- **A `#GP` with `rip` a small integer**, once, in the same setting: the fault
  is inside the TLB-shootdown IPI handler and the return address on the kernel
  stack had been overwritten. Same shape as the two-CPUs-one-stack family,
  which the lease CAS was meant to close, so either that fix is incomplete or
  something else writes into a kernel stack. The switch path now refuses to
  run a task that is already some CPU's current task -- a walk of at most
  MAX_CPUS pointers per switch -- so if it is the two-CPUs-one-stack shape the
  next occurrence panics with both CPU numbers instead of faulting at a wild
  address.
- **A task switched out holding a linuxkpi spinlock.** Once, in an `all`
  workload at `SMP=6` and scale 400: the per-CPU held-lock count was still set
  from another task (`filemap_find_get`'s xarray lock, taken by task 254) when
  a second task yielded on that CPU, and the guard panicked
  ("yield while holding an lkpi spinlock").

  The count was the bug. A release credited itself to the CPU doing the
  releasing, and a lock released on a different CPU than it was taken on --
  which the tree already reports, and which happens -- left the acquiring
  CPU's count positive for the rest of the boot. Everything keyed on that
  count then described a machine that did not exist: the next task to yield
  there was panicked for a lock it never held, and the interrupt-restore path
  stopped firing because the count never reached zero again. A release is now
  credited to the CPU that took the lock.
- **Memory pressure.** The panics are gone -- heap growth, page-table
  allocation, `fork` and large allocations report ENOMEM and the OOM killer
  runs -- but fsverify at 384 MiB with scale 200 still does not finish: usually
  the guest degrades honestly (btrfs aborts its transaction with ENOMEM, fork
  fails), and once it ended in a TLB-shootdown stall
  (`tlb: STUCK cpu N task=kswapd`), which was not reproduced again. Belongs to
  M127.
