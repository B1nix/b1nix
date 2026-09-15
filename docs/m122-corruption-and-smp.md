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

Result after the fixes: 8/8 at 1, 2, 4 and 6 CPUs with a 1 GiB guest and up to
1080 files per filesystem, and 11/12 earlier at 384 MiB (the twelfth was an OOM,
see "Open").

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
- **Task claim.** A CPU now claims a READY task by CAS on its kernel-stack
  lease, which `arch_context_switch` publishes only after the context is saved;
  only the CAS winner may load the context. 30/30 `SMP=6` vm-none/vm/spawn soak
  runs clean.

## Not closed

- **klarge double free at PID 1.** Not reproduced in any run since the PMM
  metadata fix (about a dozen full smokes and the soak matrix). That corruption
  is a sufficient cause, but the original report was never reproduced on
  demand, so the link is probable rather than shown.
- **Task-claim proof.** `sched_handoff_recover` can still publish a lease for a
  task that did not switch out; the ordering argument holds only for the paths
  that do not use it.
- **i915 `uncore->lock` lockup.** 0/30 probe boots on the fixed kernel and 0/30
  on the kernel before these fixes: the lockup does not reproduce at all now,
  so no fix can be attributed.
- **OOM under memory pressure panics** (`vmm: OOM during page table
  allocation`, `kheap: OOM during heap growth`) instead of failing the
  allocation: fsverify at 384 MiB with scale 200. Belongs to M127.
