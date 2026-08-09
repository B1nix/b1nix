/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_STDDEF_H
#define LKPI_LINUX_STDDEF_H
#include <b1nix/types.h>
#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
/* A flexible array in the middle of a struct, spelled the way imported source
 * spells it. */
#define DECLARE_FLEX_ARRAY(type, name) \
	struct { struct { } __empty_##name; type name[]; }
#endif
