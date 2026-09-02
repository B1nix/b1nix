# The virtgpu adapter: Mesa on the host GPU, through our DRM node

M101's accelerated path. `docs/render-path.md` describes what was missing: an
adapter putting b1nix's working VirGL transport behind the Linux DRM ioctl
numbers, with each resource backed by a GEM object so a `bo_handle` means
something. That adapter now exists.

## What works, proved by the machine

With a `virtio-gpu-gl-pci` device, `/bin/gl_probe` gets this far:

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

## Four defects found bringing it up, all fixed

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

## Three more defects, and the flag is gone

**A GEM object was freed without releasing its mmap offset.** The driver's
`free` called `drm_gem_private_object_fini()`, which only tears down the
reservation object. The object also owns a `drm_mm` node — its mmap offset —
living *inside* the struct and linked into the device's offset manager, and
`drm_gem_object_release()` is what takes it back out. Freeing without it left a
node of freed heap in the manager's tree; the next object to ask for an offset
descended into it, read a pointer out of reused memory and took a #GP inside
`drm_mm`'s hole-size tree.

That is what "the second mmap offset faults" was. The fault was in imported
code and the cause was in the driver, exactly as the standing rule says — but
it was NOT the augmented rbtree: `drm_mm` and the offset manager are both
exercised at boot now (`M101-IMPORT: ok drm-mm`, `ok drm-vma-manager`) and both
were correct all along.

**The host-side unref freed the object's pages a second time.** A resource
created for the DRM node is backed by a GEM object's scattered shmem pages;
`vgpu_res_unref_id()` freed `size / PAGE_SIZE` frames starting at the first
one, which both double-freed the pages the object still owned and handed the
allocator frames belonging to somebody else — a heap corruption that surfaced
minutes later as a broken free-list link. The resource table now records who
owns the frames: the character device owns the contiguous run it allocates, a
DRM resource does not.

**`execbuffer` ignored `VIRTGPU_EXECBUF_FENCE_FD_OUT`.** It answered 0 and left
`fence_fd` as userspace passed it in, so Mesa waited on a descriptor that was
never a fence. Submission here is synchronous — the command carries a fence and
the transport waits for the host to retire it — so the out-fence is signalled
the moment it exists, and that is what is handed back. Unknown flags are now
refused rather than silently accepted.

## The flag

The adapter is on by default: `lkpi_virgl_available()` decides, and a plain
virtio-gpu with no virglrenderer behind it still reports "no 3D" and gets a
clean software fallback. `b1nix.no-virgl-drm` forces it off, for bisecting a
rendering fault against the software path without rebuilding.

`b1nix.virgl-trace` prints each ioctl and its answer, because a client reports
one line for a dozen possible causes.

## Proved end to end

With a `virtio-gpu-gl-pci` device and an image carrying a Mesa DRI driver
(`make B1NIX_GPU_DRV=1 iso-gfx`), the renderer smoke selects the accelerated
path on its own and the compositor's frames come back through wlr-screencopy
with the colours it was told to paint:

```
RENDER-SMOKE: accel-status available device=/dev/dri/renderD128
              gl=virgl (AMD Radeon RX 6600 (radeonsi, navi23, ACO, ...))
RENDER-SMOKE: ok selection renderer=gles2 mode=accelerated reason=gl-probe-passed
RENDER-SMOKE: ok accel-frame
```

Run it with:

```sh
make B1NIX_GPU_DRV=1 iso-gfx
SKIP_BUILD=1 SMOKE_INSTANCES=gfx GPU_DEVICE=virtio-gpu-gl-pci \
    GPU_DISPLAY=egl-headless sh tests/smoke.sh x86_64
```

The ordinary image carries no DRI driver (mesa-dri-gallium is 184 MB), so the
default suite reports `accel-frame` as a skip with the reason on the record.
