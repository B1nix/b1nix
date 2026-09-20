# Documentation

b1nix is two things, and the documentation is split along that seam:

- **[kernel/](kernel/)** — the kernel itself: what it implements, how it is
  built and tested, and which milestone owns what.
- **[distro/](distro/)** — the distribution built on top of it: a Debian
  derivative with its own kernel, its own apt overlay and public releases.

The two have separate roadmaps because they move at different speeds. A kernel
milestone lands when it is proved by the smoke suite; a distribution phase
lands when something is installable. They meet in
[versioning.md](versioning.md), which says how the four version numbers relate.

| Where | What is in it |
|---|---|
| [kernel/roadmap.md](kernel/roadmap.md) | Kernel milestones, open and closed |
| [kernel/platforms.md](kernel/platforms.md) | Targets, AArch64, distribution userspace lanes |
| [kernel/memory-and-scheduling.md](kernel/memory-and-scheduling.md) | Memory, scheduling, SMP |
| [kernel/processes-and-system-calls.md](kernel/processes-and-system-calls.md) | Processes, signals, the Linux ABI surface |
| [kernel/filesystems-and-storage.md](kernel/filesystems-and-storage.md) | VFS, filesystems, block layer |
| [kernel/networking.md](kernel/networking.md) | Network stack and drivers |
| [kernel/isolation-and-security.md](kernel/isolation-and-security.md) | Namespaces, capabilities, keys, hardening |
| [kernel/drivers-and-graphics.md](kernel/drivers-and-graphics.md) | linuxkpi, DRM, i915, the desktop path |
| [kernel/build-conventions.md](kernel/build-conventions.md) | Build and smoke conventions |
| [kernel/xperia5-ufs-usb.md](kernel/xperia5-ufs-usb.md) | The SM8150 phone target |
| [distro/roadmap.md](distro/roadmap.md) | Distribution phases and releases |
| [distro/plan.md](distro/plan.md) | The full distribution plan, including what gets cut |
| [versioning.md](versioning.md) | Kernel, ABI, release and package versions |
