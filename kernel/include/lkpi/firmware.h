/* SPDX-License-Identifier: MIT */
#ifndef LKPI_FIRMWARE_H
#define LKPI_FIRMWARE_H

#include <lkpi/types.h>

/*
 * request_firmware — load a firmware blob from the filesystem.
 *
 * Linux has a user-helper fallback and a built-in blob table; b1nix has one
 * source, the VFS, searched under the standard paths. amdgpu will not
 * initialise at all without this (PSP refuses to hand the GPU over until it has
 * verified microcode), so the path list matches what those drivers ask for.
 *
 * Sleeps: reads a file. Never call from interrupt context or under a spinlock.
 */

struct firmware {
	const u8 *data;
	usize size;
};

/* Directories searched, in order, when `name` is not absolute. */
#define LKPI_FIRMWARE_PATH_0 "/lib/firmware/"
#define LKPI_FIRMWARE_PATH_1 "/usr/lib/firmware/"

/* Load `name`. On success *fw points at a heap copy owned by the caller until
 * release_firmware(). Returns 0, -ENOENT when no path matched, -ENOMEM, or
 * -EIO on a short read. */
/* `dev` is the device the blob belongs to. Upstream uses it to build the search
 * path and to log against the right device; b1nix loads from a fixed path and
 * logs to the kernel log, so it is recorded and not otherwise used — but it is
 * in the signature because every caller passes it, and a two-argument version
 * meant every imported call site was a compile error. */
struct device;
int request_firmware(const struct firmware **fw, const char *name,
                     struct device *dev);

/* Same, but does not log when the blob is missing — for optional firmware. */
int firmware_request_nowarn(const struct firmware **fw, const char *name,
                            struct device *dev);

void release_firmware(const struct firmware *fw);


/* Load without falling back to the userspace helper. There is no helper here —
 * b1nix loads from the filesystem directly — so this is the ordinary request. */
#define request_firmware_direct(fw, name, dev) firmware_request_nowarn(fw, name, dev)

#endif
