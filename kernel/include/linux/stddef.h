/* SPDX-License-Identifier: GPL-2.0-only */
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
/* The uapi spelling, which on-disk format headers use. */
#define __DECLARE_FLEX_ARRAY(type, name) DECLARE_FLEX_ARRAY(type, name)

/* The offset just past a member — offsetof plus its size. Used for the
 * compile-time layout assertions upstream puts next to structures two code
 * paths share; getting it wrong there is exactly the "a cast is not a layout
 * guarantee" failure those assertions exist to catch. */
#ifndef offsetofend
#define offsetofend(TYPE, MEMBER) \
	(offsetof(TYPE, MEMBER) + sizeof(((TYPE *)0)->MEMBER))
#endif

/*
 * The size of one member of a type, without an instance of it. Used all over
 * the filesystems to check that an on-disk field is the width they think it is,
 * inside static_assert.
 */
#ifndef sizeof_field
#define sizeof_field(TYPE, MEMBER) sizeof((((TYPE *)0)->MEMBER))
#endif
#ifndef offsetofend
#define offsetofend(TYPE, MEMBER) \
	(offsetof(TYPE, MEMBER) + sizeof_field(TYPE, MEMBER))
#endif

#endif
