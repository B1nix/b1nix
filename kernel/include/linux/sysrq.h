/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_SYSRQ_H
#define LKPI_LINUX_SYSRQ_H
/* The magic-key handler DRM registers so a wedged machine can be forced back to
 * text mode. b1nix has no sysrq input path, so registration is a no-op. */
struct sysrq_key_op;
static inline int register_sysrq_key(int key, const struct sysrq_key_op *op)
{ (void)key; (void)op; return 0; }
static inline int unregister_sysrq_key(int key, const struct sysrq_key_op *op)
{ (void)key; (void)op; return 0; }
#endif
