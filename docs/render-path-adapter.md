# The virtgpu adapter: what Mesa now reaches, and what stops it

M101's accelerated path. `docs/render-path.md` describes what was missing: an
adapter putting b1nix's working VirGL transport behind the Linux DRM ioctl
numbers, with each resource backed by a GEM object so a `bo_handle` means
something. That adapter now exists.

## What works, proved by the machine

With `b1nix.virgl-drm` on the command line and a `virtio-gpu-gl-pci` device,
`/bin/gl_probe` gets this far:

```
GL-PROBE: renderer virgl (AMD Radeon RX 6600 (radeonsi, navi23, ACO, ...))
GL-PROBE: ok hardware-renderer
GL-PROBE: ok shader-compile-and-link
GL-PROBE: ok framebuffer 64x64
drm: virgl: execbuffer 4620 bytes, 1 bo handles
drm: virgl: execbuffer -> 0
```

That is a Mesa GL context running on the host's real GPU, reached through our
DRM node: parameters answered, capset served, resources created and attached,
and a virgl command stream submitted and accepted by the host. The probe's own
`hardware-renderer` check rejects llvmpipe and swrast, so the renderer string
is evidence rather than decoration.

## Four defects found on the way, all fixed

**GETPARAM writes THROUGH the pointer.** `drm_virtgpu_getparam.value` is a user
pointer and the kernel copies a 4-byte `int` to it; filling the struct field
instead makes every query "succeed" while the caller reads its own untouched
stack. Mesa was therefore told by its own zeroed variable that there was no 3D,
and gave up before asking anything else — reporting only "failed to create dri2
screen". Direction and size both matter: an int, not a u64.

**The capset is found by id, not by index.** The host numbers capsets by index
and names them by id, and the two do not agree: VIRGL is index 0, VIRGL2 is
index 1. Mesa asks for VIRGL2. Looking only at index 0 turned "you asked for a
capset I have" into "the host refused".

**A command could not exceed one page.** `virtio_gpu_send_cmd` rejected any
request longer than `PAGE_SIZE`, and a virgl command stream is routinely
larger — Mesa's first submission here is 4620 bytes. The command never reached
the host and the caller saw an untouched response buffer: a zero where a
refusal code would have been. The control request buffer is now 16 pages.

**The device round trip ran with interrupts disabled.** The scheduler's job
callback held a `spin_lock_irqsave` across a full command submission. While the
host was rejecting oversized commands instantly that was merely wrong; once the
host began doing real work, the completion interrupt could not be delivered and
the timer could not tick, and the guest stopped dead after the first command it
actually executed. Serialisation on the shared submit buffer now yields instead
of masking interrupts.

## What stops it, precisely

`drm_gem_create_mmap_offset()` on the **second** GEM object faults:

```
#EXC vec=0xd  rip=drm_mm.c:314 best_hole
                <- first_hole (drm_mm.c:363)
                <- drm_mm_insert_node_in_range (drm_mm.c:540)
```

A general protection fault — a non-canonical pointer read out of the tree, not
a missing page. The first insert cannot expose it, because an empty tree is
never traversed.

This is the **first real use of upstream's `drm_mm`** in b1nix: nothing else
inserts more than one node into the range allocator, which is why it has stood
until now. By the project's standing rule a fault inside imported code is a
defect in the shim, so the fix belongs in the augmented rbtree
(`RB_DECLARE_CALLBACKS_MAX`, `rb_insert_augmented`, `rb_root_cached`), not in
`drm_mm.c`.

## Why the path is behind a flag

`b1nix.virgl-drm` gates the whole adapter. With it off, `GETPARAM` answers "no
3D" and Mesa falls back to software cleanly — the designed behaviour, and safe.
With it on, the path runs for whoever is working on it. Answering "yes" by
default while knowing the second buffer takes the machine down would be the
worst of the three, and a green smoke run bought that way would be a lie.

`b1nix.virgl-trace` prints each ioctl and its answer, because a client reports
one line for a dozen possible causes.
