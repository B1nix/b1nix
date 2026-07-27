#ifndef B1NIX_FTRACE_H
#define B1NIX_FTRACE_H

#include <b1nix/types.h>

/* ftrace — function entry/exit tracing (M36).
 *
 * Files compiled with -finstrument-functions call __cyg_profile_func_enter /
 * __cyg_profile_func_exit, which record (function address, enter|exit) into a
 * ring buffer when tracing is enabled. ksym_lookup symbolises the addresses on
 * dump. Only opted-in files are instrumented (see the Makefile), so the kernel
 * is not globally slowed and the hooks never recurse on themselves. */

#define FTRACE_ENTER 0
#define FTRACE_EXIT  1

struct ftrace_event {
  u64 addr; /* instrumented function address */
  u32 seq;  /* monotonic record sequence */
  u8 type;  /* FTRACE_ENTER / FTRACE_EXIT */
};

void ftrace_enable(void);
void ftrace_disable(void);
void ftrace_reset(void);
usize ftrace_count(void);
const struct ftrace_event *ftrace_get(usize i);
void ftrace_dump(void); /* symbolised dump to console */

/* In-kernel self-test (M36 smoke). Called from main.c in test mode. */
void m36_ftrace_selftest(void);

#endif
