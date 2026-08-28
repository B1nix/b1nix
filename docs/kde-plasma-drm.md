# Plasma on the real DRM path

kwin_wayland now modesets `/dev/dri/card1` on b1nix and plasmashell paints on
it, with no compositor underneath. The picture is
[`images/m113-plasma-drm.png`](images/m113-plasma-drm.png), taken from the host
with the QEMU monitor's `screendump`, and `tests/kde-smoke.sh` is what decides
whether a run reproduced it.

```sh
B1NIX_KDE=1 KERNEL_CMDLINE="b1nix.kde b1nix.trace-sysfs" make -j6 iso
sh tests/kde-smoke.sh            # KDE_NO_BUILD=1 to reuse the image as it is
```

## What was actually wrong

The previous handoff left the chain stopped at one place: `TakeControl`
succeeded, `TakeDevice(226, 1)` answered `System.Error.ENODEV`, and a kernel
trace of the whole boot showed logind never reaching an `open()` of the node.
Two separate defects were behind that, and neither was in the graphics stack.

### 1. `O_PATH` and `O_NOFOLLOW` were dropped, so every symlink was followed

`linux_open_flags_to_b1nix` translated Linux open flags through an explicit
whitelist and discarded these two. That looks harmless — a flag that only
restricts an open — until you notice how a path is walked by hand.

systemd's `chase()` resolves a path one component at a time: it opens each with
`O_PATH|O_NOFOLLOW`, calls `fstat`, and either reads a symlink with `readlink`
and splices the target into the walk, or descends into a directory. Every
`sd_device` lookup goes through it, so all of logind does.

With the flags dropped, `openat("/sys/dev/char/226:1", O_PATH|O_NOFOLLOW)`
followed the link and `fstat` described the *directory behind it*. The walk
concluded it had reached a directory, stopped there, and took the link's own
path as canonical. `sysname` — the last component of the resolved path — came
out as `226:1` instead of `card1`, so logind's `detect_device_type` found
neither a `card*` DRM minor nor an `event*` input device and returned
`-ENODEV`. It never opened anything because it had already decided the device
was not a graphics device.

The kernel trace says it exactly: elogind opens `/sys`, `/sys/dev`,
`/sys/dev/char`, `/sys/dev/char/226:1` and its `uevent` — and never
`/sys/devices/...`, which is where the link points.

`O_NOFOLLOW` now refuses a final symlink with `ELOOP`; with `O_PATH` as well the
descriptor refers to the link itself and `fstat` reports `S_IFLNK`. An `O_PATH`
descriptor has no contents, so reads and writes are `EBADF`, and it still
anchors `openat()`. Covered by four checks in `M17-SMOKE`.

### 2. Nothing wrote the udev database, so the card belonged to no seat

logind hands a device to a session only when the device carries udev's `seat`
tag, and tags come from `/run/udev/data/c226:1` and nowhere else — a missing
database file is read as "no tags" in silence. The KDE image shipped `libudev`
and `libudev-zero` but no udev **daemon**, so that file never existed.

The image now stages **eudev**. Its udevd runs the seat rules elogind already
installs (`/lib/udev/rules.d/71-seat.rules`, `73-seat-late.rules`), which is
where `TAG+="seat"` and `TAG+="master-of-seat"` for `SUBSYSTEM=="drm",
KERNEL=="card*"` come from. No rule text is written by hand.

For the replay to reach the cards, two kernel gaps had to close:

- **The DRM `uevent` files were read-only.** `udevadm trigger` coldplugs by
  writing `add` to every `/sys/**/uevent`, and a file that refuses the write is
  a device no replay can announce. They are `0644` now with a store that really
  re-announces, carrying `DEVTYPE=drm_minor` — an `sd_device` built from a
  netlink message is sealed, so a rule asking for the devtype gets no answer
  unless the announcement carries it.
- **The DRM `subsystem` symlink had one `../` too many**, resolving to a path
  that exists nowhere.

The result, from the guest:

```
seat0
        Sessions: *c1
         Devices:
                  └─/sys/devices/pci0000:00/0000:00:03.0/drm/card1
                    [MASTER] drm:card1
CanGraphical=true
```

### 3. `utimensat` refused the path-less form, and udevd spun on it

`utimensat(fd, NULL, ...)` *is* `futimens(3)`; the kernel answered `EINVAL`.
udevd touches `/run/udev/queue` that way after each event, treated the refusal
as retryable, and looped on it forever — printing `could not touch
/run/udev/queue: Invalid argument` and starving everything else on the machine.
A daemon spinning is what an unimplemented syscall looks like from outside.
Relative paths against a directory descriptor were rejected with `EBADF` too;
both forms work now.

### 4. A page cache that freed entries its readers still held

Found once Plasma ran long enough to put the cache under pressure. Readers pin
an entry in `page_cache_get_page` under the entry's **bucket** lock, while every
evictor tested `refcount` under **`pc_lock`** — two locks guarding the same
word, so they never excluded each other. Between an evictor's test and its
unlink, a reader could find the entry, pin it, and return the pointer; the
evictor then freed the frame and deferred the entry to `kfree` while the reader
still held it.

It surfaced as a `#GP` in `node_read_impl`'s `memcpy` whose source address was
not an address at all but the reused bytes of the freed entry
(`0xf0006987f000fea5` — the BIOS fill pattern, and not page-aligned, which no
stale-but-real frame could be). The same window let a freed frame be installed
into a user PTE from the fault path, which is worse than a crash.

Victims are now claimed under the bucket lock and abandoned if a reader got
there first; `refcount` and `flags` are updated atomically since they are
touched from both lock domains. This was a regression from the per-bucket-lock
change — before it, reader and evictor were both under `pc_lock` and the
invariant held.

## The harness, and why it is shaped this way

Three things are asserted, each one the guest cannot forge:

1. **The frame.** `screendump` reads the framebuffer the guest programmed into
   the virtual GPU; no process inside the guest produces the file. A display
   that never drew dumps a solid rectangle — two or three distinct colours. The
   desktop above holds ~45000. The threshold is on that count, not on a marker.
2. **The device open.** `b1nix.trace-sysfs` records every open under `/dev/dri`
   with the task that made it, from inside the kernel. `elogind` opening
   `card1` is in that trace.
3. **No compositor underneath.** The run must have taken the `drm` backend and
   no wlroots compositor may be running — a desktop nested in sway proves KDE's
   clients work, not that KDE drives a display.

### Photograph the right device

QEMU adds a standard VGA adapter alongside the `virtio-gpu-pci` the guest
draws on, and a bare `screendump` takes the first one. Every picture was
therefore of the VGA console — 94% black with one other colour, which reads
exactly like a desktop that never painted, and cost a full debugging cycle.
`tools/run-kde.sh` names the device now (`screendump <file> vgpu`).

## 5. The memory, and the time

Both questions had the same shape: a number that looked like Plasma's cost and
was not.

### 8 GB was ours: the allocator never split a block it reused

`/proc/meminfo` reported four numbers, and `MemAvailable` was a copy of
`MemFree` — enough to see that memory was gone, never where. With Cached,
Dirty, AnonPages and Slab added, one read answered it:

| | kwin start | +3 min |
|---|---|---|
| Cached | 173 MB | 278 MB — flat |
| AnonPages | 2.88 GB | 3.04 GB — flat |
| **Slab (kernel heap)** | **621 MB** | **1516 MB, still climbing** |

`/proc/b1nix-kheap` ruled out fragmentation: **live 1483 MB, free 11 MB**, in
301387 blocks averaging 4.9 kB each — in a kernel whose objects are mostly
tens of bytes.

The cause is four lines in `kmalloc_internal`. Reuse takes the first free block
big enough and hands it over whole:

```c
if (cur->size >= size) { *prev = cur->next; cur->magic = KHEAP_MAGIC; block = cur; break; }
```

`cur->size` is never reduced, so a 64-byte allocation that lands on a 512 KB
free block consumes all 512 KB permanently. Coalescing turns that into a
ratchet — adjacent frees merge into ever larger blocks, each swallowed whole by
the next small allocation that reaches it. The comment above the coalescing
code even says merging "undoes the fragmentation that split-on-reuse otherwise
leaves behind": the split was intended and never written.

With `kheap_split_block` carving the tail back into a free block (fixing the
`prev_size` boundary tags and `heap.last_block` as it goes), the same workload
holds **104 MB and stops growing**, and Plasma runs in 4 GB again.
`KHEAP-SELFTEST` covers it, because nothing else would: the bug corrupted
nothing and failed nothing, it only ran the machine out of memory later.

### 165s → 63s, and ninety of those seconds were this file

The wait for plasmashell to paint grepped its log for strings this build never
prints, so it always ran to its ninety-second cap — while the desktop was on
screen the whole time, as the photograph shows. That is the second time the
same check has been wrong the same way, and both times the number it produced
was read as Plasma being slow. It now waits for kwin's own log of plasmashell
binding globals: one process observing another, rather than a program's
account of itself.

Of the ~63s that remain, ~20s is `udevadm settle` reaching its timeout because
udev workers stall (an open M112 item), ~8s is boot, and the rest is genuine
start-up under software rendering.

## Open

- **kwin falls back to the legacy modeset path.** It asks for
  `DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT` (capability 6), added in Linux 6.7,
  and the imported core is 6.6 — so the refusal is correct and kwin handles
  it. `b1nix.drm-debug` now names the capability behind a refused
  `SET_CLIENT_CAP` rather than printing the ioctl number alone. Universal
  planes are still not offered; legacy modesetting works.
- **udev workers time out** on the DRM devices after the desktop is up
  (`worker [186] ... timeout; kill it`). The tagging has already happened by
  then, so it does not affect the seat, but a worker that never exits is a
  real fault, and it is the same stall that costs 20s at `udevadm settle`.
