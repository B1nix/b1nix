# linuxkpi — the primitives an imported DRM driver stands on

The vendor drivers are imported as upstream writes them and are never edited.
Everything underneath them is ours, written from scratch under MIT. This
document records what that layer provides and, for each piece, the property the
self-test actually asserts — a primitive that merely compiles is not a
primitive a driver can trust.

Milestone: **M101** (roadmap). The import itself — which tree, which release,
how it is staged and pinned — is [`drm-import.md`](drm-import.md).

## Lifetime and reference counting

- **`kref`**: only the last put releases, and taking a weak reference on a dead
  object fails rather than resurrecting it.
- **`kobject`**: release runs once, on the last put, and a child's release runs
  before its parent's reference is dropped.
- **`pm_runtime`**: usage-counted; suspends only on the last holder, refuses to
  claim a suspend the driver rejected, and leaves no reference behind on a
  failed resume.

## Sleeping and mutual exclusion

- **Wait queues** over the two-phase scheduler wait, with timeouts measured
  against the scheduler's own ticks.
- **`ww_mutex`** with real wound-wait: an older context wounds a younger holder,
  the younger is refused with `EDEADLK` and backs off, and the older is never
  wounded itself.
- **`kthread_worker`**: a queue whose thread the caller owns, running its items
  in submission order.
- **RCU** with honest grace periods: readers are counted in two buckets, a
  writer flips which bucket is current and waits for the old one to drain. Read
  sections disable interrupts, so they cannot sleep or migrate — the header
  states that as the price. The grace period is *proved*, not assumed: a reader
  on another CPU keeps re-reading an object that the writer poisons the instant
  `synchronize_rcu` returns, so an early grace period is reported by the reader
  instead of corrupting memory quietly.

## Data structures

- **Red-black trees** with balance verified rather than inferred: ascending
  inserts stay logarithmic instead of degenerating into a list.
- **An augmentation hook** on the rbtree, so a field derived from a whole
  subtree survives rebalancing rather than going quietly stale.
- **Interval trees** on top of it — the structure that answers "which ranges
  cover this address" — checked against a brute-force scan.
- **`xarray`**: sparse 64-bit index to pointer, ordered iteration, folding back
  to genuinely empty on erase.

## Memory

- **`struct page`** with a backing our memory model can honour: page arrays,
  shmem-backed objects, `vmap`, and write-combining through the M98 PAT paths.
  There is no global `mem_map` — a page is allocated with its frame, so there is
  no physical-address-to-page lookup, and the header says so.
- **The scatter is real**: a shmem array takes its frames one at a time and the
  self-test asserts they are *not* one physical run, so a driver that assumes
  `page[i+1]` follows `page[i]` breaks here rather than on hardware with the
  IOMMU switched off.

## Presenting the device to userspace

- **`sysfs`/`debugfs` attribute files** through a registry that accepts
  registrations before `/sys` is mounted and materialises them when it is, so
  probe order and mount order need not agree. Classes, devices, attribute groups
  and links are real; removing one file does not disturb its siblings and does
  release the caller's context. A debugfs file gets the `seq_file` it is written
  against (`single_open`/`seq_read`, rendered once into a buffer that grows until
  the whole dump fits), so a read past the first buffer continues rather than
  restarting. Registering a device broadcasts a uevent on
  `NETLINK_KOBJECT_UEVENT` that a bound listener receives. Every part is proved
  by reading it back the way userspace would.

## What the layer was proved against

- The DRM core builds unmodified: all 41 objects of upstream's own `drm-y` list,
  plus the MIT hdmi infoframe library, with nothing edited under the staged
  tree. Every fix went into the shim.
- A driver on the imported core registers a `drm_device`, a connector and a
  simple display pipe, and serves dumb buffers as GEM objects with handles.
- The in-kernel DRM client probes the connector, allocates a framebuffer and
  commits it through upstream's atomic helpers — and the pixels are read back
  off virtio-gpu's scanout (corners and centre), so a commit that returns
  success without moving an image fails the test.
- `/dev/dri/card1` is served by upstream's own `drm_open`/`drm_ioctl`/
  `drm_read`/`drm_poll`, with dumb buffers mapped through `drm_vma_manager`:
  offsets resolved a page at a time and refused when `drm_vma_node_is_allowed`
  says the client does not own the object. A userspace test runs the sequence
  libdrm runs, against the *pinned* uapi headers rather than a copy, and checks
  that the pattern it painted came out the far end of the commit. Two nodes
  exist on purpose — the new surface is proved on its own before anything moves
  onto it.

**The standing rule:** a fault seen in imported code is a defect in this layer.
The fix goes here, never into the import tree.
