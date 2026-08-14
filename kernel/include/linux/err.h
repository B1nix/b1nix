/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_ERR_H
#define LKPI_LINUX_ERR_H
#include <linux/types.h>
#include <lkpi/types.h>
/* Errors returned in pointer position. The top page of the address space is
 * never a valid object, so a small negative value there is unambiguous. */
/*
 * b1nix's <b1nix/errno.h> already defines ERR_PTR/PTR_ERR/IS_ERR as macros with
 * the same meaning. Its header is included first (through <linux/errno.h>) and
 * the macros are dropped here so these can be real functions — typed, so a
 * caller cannot pass an int where a pointer belongs and have it silently work.
 */
#undef ERR_PTR
#undef PTR_ERR
#undef IS_ERR
#undef IS_ERR_OR_NULL

#define MAX_ERRNO 4095
#define IS_ERR_VALUE(x) ((usize)(void *)(x) >= (usize)-MAX_ERRNO)
static inline void *ERR_PTR(long error) { return (void *)(usize)error; }
static inline long PTR_ERR(const void *ptr) { return (long)(usize)ptr; }
static inline bool IS_ERR(const void *ptr) { return IS_ERR_VALUE((usize)ptr); }
static inline bool IS_ERR_OR_NULL(const void *ptr)
{ return !ptr || IS_ERR(ptr); }
static inline void *ERR_CAST(const void *ptr) { return (void *)ptr; }
static inline int PTR_ERR_OR_ZERO(const void *ptr)
{ return IS_ERR(ptr) ? (int)PTR_ERR(ptr) : 0; }
#endif
