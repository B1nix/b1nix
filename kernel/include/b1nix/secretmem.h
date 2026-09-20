/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_SECRETMEM_H
#define B1NIX_SECRETMEM_H
/* memfd_secret(2) — see kernel/mm/secretmem.c. */
#include <b1nix/types.h>

struct task;
struct vfs_node;
struct vm_area;

void secretmem_init(void);
/* 0 when the pool could not reserve its scrub window: memfd_secret is then
 * unavailable, as on a Linux kernel booted without secretmem. */
int secretmem_available(void);
/* Make a fresh file node a secret-memory file. */
int secretmem_attach(struct vfs_node *node);
/* Is this frame one the kernel's own mappings no longer reach? Code that reads
 * another process's memory through the direct map must ask first. */
int secretmem_frame_is_hidden(u64 frame);
int secretmem_vma(const struct vm_area *v);
int secretmem_mmap_check(struct task *t, u64 length, int map_flags);
#endif
