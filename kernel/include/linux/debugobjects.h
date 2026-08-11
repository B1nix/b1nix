/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_DEBUGOBJECTS_H
#define LKPI_LINUX_DEBUGOBJECTS_H
#include <linux/types.h>
/* Object-lifetime debugging: a registry that catches "initialised twice" and
 * "freed while active". b1nix does not carry it, so the tracking calls compile
 * to nothing. This costs a class of diagnostics, not correctness. */
#define debug_object_init(addr, descr)        do { (void)(addr); } while (0)
#define debug_object_activate(addr, descr)    ({ (void)(addr); 0; })
#define debug_object_deactivate(addr, descr)  do { (void)(addr); } while (0)
#define debug_object_destroy(addr, descr)     do { (void)(addr); } while (0)
#define debug_object_free(addr, descr)        do { (void)(addr); } while (0)
#define debug_object_assert_init(addr, descr) do { (void)(addr); } while (0)

/* The descriptor a caller registers with the tracker. There is no tracker here
 * (see above), but callers define one as a static const, so the type has to be
 * complete or the definition does not compile. */
struct debug_obj;
enum debug_obj_state { ODEBUG_STATE_NOTAVAILABLE };
struct debug_obj_descr {
	const char *name;
	void *(*debug_hint)(void *addr);
	bool (*is_static_object)(void *addr);
	bool (*fixup_init)(void *addr, enum debug_obj_state state);
	bool (*fixup_activate)(void *addr, enum debug_obj_state state);
	bool (*fixup_destroy)(void *addr, enum debug_obj_state state);
	bool (*fixup_free)(void *addr, enum debug_obj_state state);
	bool (*fixup_assert_init)(void *addr, enum debug_obj_state state);
};


/* The on-stack and state-assertion variants. Same absence as the rest of this
 * header. */
#define debug_object_init_on_stack(addr, descr) do { (void)(addr); } while (0)
#define debug_object_active_state(addr, descr, expect, next) \
	do { (void)(addr); (void)(expect); (void)(next); } while (0)

#endif
