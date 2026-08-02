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
int request_firmware(const struct firmware **fw, const char *name);

/* Same, but does not log when the blob is missing — for optional firmware. */
int firmware_request_nowarn(const struct firmware **fw, const char *name);

void release_firmware(const struct firmware *fw);

#endif
