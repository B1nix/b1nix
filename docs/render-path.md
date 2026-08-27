# The accelerated render path

M101's second half — hardware rendering as a path *beside* the software one,
never as a replacement. The software path stays first-class: acceleration is
claimed only when a render node opens, a Mesa DRI driver is present, and
`gl_probe` draws a shader triangle on it and reads the pixels back. Otherwise a
compositor starts on pixman rather than failing.

`RENDER-SMOKE: ok accel-frame` has never been produced. This is what was in the
way, what has been removed, and what is left.

## Two blockers, both removed

**The driver had the wrong name.** `DRM_IOCTL_VERSION` reported `b1nix`. That
name is the first thing Mesa's loader matches on, ahead of the PCI id, and it
looks for `<name>_dri.so` — so every client went hunting for a `b1nix_dri.so`
that does not exist and never will, while the driver for the hardware actually
behind the device sat unused in the image. The device is a virtio GPU; it
reports `virtio_gpu` now, and the uevent's `DRIVER=` line names the driver
rather than the bus it sits on.

**The card had no PCI identity.** Its parent was a bare `struct device`.
`to_pci_dev()` is a `container_of`: it cannot fail and it cannot check, so
whatever sat next to that struct in memory was read as vendor, device and slot,
and b1nix published the result in sysfs as fact — `0000:0000`. Mesa refuses to
match a driver against that, so even forcing `iris` was declined.

The fix is not a better guess at the reading end. The DRM device's parent is now
a real `struct pci_dev`, filled from b1nix's own enumerator with virtio-gpu's
bus address and its real `1af4:1050`. Every `pci_dev` the shim constructs
carries a stamp, and the code that publishes an identity asks whether a parent
is a PCI function instead of assuming it. A parent that is not one has no PCI
identity, and saying so lets the caller fall back rather than invent one.

`M101T-DRM: ok pci-identity` reads `/sys/class/drm/card1/device/{vendor,device}`
back and fails on anything that is not virtio-gpu's, so a regression here is a
test failure rather than a silent return to `0000:0000`.

## A third, found on the way: the master lease

`capable()` in the shim answered "not privileged" unconditionally. That is the
safe answer for a driver deciding what to attempt on its own behalf, and the
wrong one for an ioctl arriving from ring 3: upstream's
`drm_master_check_perm()` asks for `CAP_SYS_ADMIN` before granting the DRM
master lease, so `DRM_IOCTL_SET_MASTER` answered `EACCES` — to root. Without the
lease no modeset ioctl is permitted, and kwin reports the whole failure as
`failed to open drm device`, naming the one step that had worked.

It now asks b1nix's own credentials, translating Linux's capability numbers to
ours (they differ: `CAP_SYS_ADMIN` is 21 there and 20 here). `M101T-DRM`
performs the entire sequence a compositor's session layer performs — `stat`,
`open` with the `O_NOCTTY|O_NONBLOCK` a session adds, the lease, and its release
— and reports each step separately, because the compositor reports all of them
as one message.

## What is left

The node serves the *generic* DRM ioctls. Mesa's virgl winsys does not speak
those: it speaks `DRM_IOCTL_VIRTGPU_*` — `GETPARAM`, `GET_CAPS`,
`RESOURCE_CREATE`, `MAP`, `EXECBUFFER`, `TRANSFER_TO/FROM_HOST`, `WAIT`,
`GEM_CLOSE` — against upstream's `virtgpu_drm.h` layouts, with GEM handles
rather than bare resource ids.

The transport those need already exists and works: `/dev/virtio-gpu` carries
b1nix's own VirGL command path (`B1NIX_VIRGL_*` in
`userspace/include/b1nix/virgl.h`), which creates resources, submits the same
virgl command stream Mesa emits, transfers in both directions and maps the
backing. What does not exist is the adapter that puts that transport behind the
Linux ioctl numbers and struct layouts on the DRM node, with each resource
backed by a GEM object so a `bo_handle` means something.

Until that adapter exists, Mesa loads `virtio_gpu_dri.so`, asks
`VIRTGPU_GETPARAM` whether 3D is available, is told the ioctl is unknown, and
falls back to software. That is the remaining piece, and it is a piece of work
rather than a defect.
