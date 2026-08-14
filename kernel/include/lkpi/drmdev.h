/* SPDX-License-Identifier: MIT */
#ifndef LKPI_DRMDEV_H
#define LKPI_DRMDEV_H

#include <b1nix/types.h>

/*
 * The character-device surface of the imported DRM core, as b1nix sees it.
 *
 * A DRM device is only half a device until userspace can open it. Upstream's
 * own file operations — drm_open, drm_ioctl, drm_read, drm_poll, drm_release —
 * are already compiled in; what was missing is the crossing from b1nix's VFS,
 * which has a different shape for every one of them: no `struct file`, no
 * `struct inode`, a poll that reports rather than sleeps, and an mmap that asks
 * for one page's physical address at a time instead of filling a VMA.
 *
 * So the bridge lives on the Linux side of the boundary (kernel/lkpi/
 * drm_chardev.c, compiled with the imported headers) and this header is what
 * the b1nix side is allowed to see: fixed-width types and opaque pointers only.
 * The same rule as <lkpi/env.h>, in the other direction — see the note there
 * for why a translation unit that sees both header sets cannot be rescued by
 * include order.
 */

/* Whether a DRM device from the imported core is registered and openable. */
int lkpi_drm_have_device(void);

/* The primary minor upstream assigned to it. Meaningful only when the above
 * returns non-zero; it is what the synthetic inode carries into drm_open, and
 * therefore what selects the device. */
u32 lkpi_drm_primary_minor(void);

/* Open flags, named here rather than shared. b1nix's O_NONBLOCK and Linux's
 * have different values, and a bridge that passed one set through as the other
 * would make a blocking read out of a non-blocking one — silently, and only
 * under load. The caller translates into these; this side translates out. */
#define LKPI_DRM_O_NONBLOCK 0x1u
#define LKPI_DRM_O_WRITE    0x2u

/*
 * Open the device. On success `*out_file` carries the Linux `struct file` the
 * core allocated — opaque here, handed back to every call below. Returns 0, or
 * a negative errno.
 */
int lkpi_drm_open(u32 minor, u32 flags, void **out_file);

/* Drop the last reference: runs the core's release and frees the file. */
void lkpi_drm_close(void *file);

/* One ioctl. `arg` is the caller's userspace pointer, passed through untouched:
 * the core copies it in and out through the uaccess shim, which validates it
 * against the calling process's address space. */
isize lkpi_drm_ioctl(void *file, u64 request, void *user_arg);
/* Record the handle a DRM file was opened through, so a clone of that file
 * (a lease) can inherit the device node. */
void lkpi_drm_file_set_handle(void *file, void *handle);
/* An independent second file on the same DRM device, for a lease. NULL on
 * failure. */
void *lkpi_drm_clone_file(void *file);
/* The minor a DRM file belongs to; 0 when the file is not a DRM file. */
int lkpi_drm_file_minor(void *file, u32 *out);

/* Read pending events into a userspace buffer. Returns bytes read, 0 when
 * nothing was ready on a non-blocking file, or a negative errno. */
isize lkpi_drm_read(void *file, void *user_buf, usize len);

/* Non-zero when a read would return an event without blocking. */
int lkpi_drm_readable(void *file);

/*
 * Resolve one page of a mapping, the way b1nix's VFS asks for it.
 *
 * `offset` is the fake mmap offset the core handed userspace out of
 * DRM_IOCTL_MODE_MAP_DUMB, so the lookup goes through upstream's own
 * drm_vma_offset_manager — and through drm_vma_node_is_allowed, which is the
 * check that keeps one client from mapping another's buffer by guessing an
 * offset. Returns 0 with the physical address in `*out_phys`, or a negative
 * errno.
 */
int lkpi_drm_mmap_page_phys(void *file, u64 offset, u64 *out_phys);

#endif
