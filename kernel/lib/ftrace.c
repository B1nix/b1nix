/* ftrace engine (M36). Holds the trace ring buffer and the GCC/clang
 * instrumentation hooks. This file is NOT compiled with -finstrument-functions
 * (so its own functions are never traced); the hooks are additionally marked
 * no_instrument_function for belt-and-braces. */

#include <b1nix/console.h>
#include <b1nix/ftrace.h>
#include <b1nix/klog.h>

#define FTRACE_CAP 256

static struct ftrace_event ring[FTRACE_CAP];
static usize ring_len;
static u32 ring_seq;
static volatile int ftrace_on;
static volatile int in_hook; /* reentrancy guard */

void ftrace_enable(void) { ftrace_on = 1; }
void ftrace_disable(void) { ftrace_on = 0; }

void ftrace_reset(void) {
  ring_len = 0;
  ring_seq = 0;
}

usize ftrace_count(void) { return ring_len; }

const struct ftrace_event *ftrace_get(usize i) {
  if (i >= ring_len)
    return 0;
  return &ring[i];
}

static void ftrace_record(u64 addr, u8 type) {
  if (!ftrace_on)
    return;
  if (__sync_lock_test_and_set(&in_hook, 1))
    return; /* already inside a hook on this path */
  if (ring_len < FTRACE_CAP) {
    ring[ring_len].addr = addr;
    ring[ring_len].seq = ring_seq++;
    ring[ring_len].type = type;
    ring_len++;
  }
  __sync_lock_release(&in_hook);
}

void ftrace_dump(void) {
  console_write("--- ftrace ---\n");
  for (usize i = 0; i < ring_len; i++) {
    console_write(ring[i].type == FTRACE_ENTER ? "  -> " : "  <- ");
    u64 off = 0;
    const char *name = ksym_lookup(ring[i].addr, &off);
    console_write(name ? name : "?");
    console_write("\n");
  }
  console_write("--- end ftrace ---\n");
}

/* ── compiler instrumentation hooks ── */
__attribute__((no_instrument_function)) void
__cyg_profile_func_enter(void *this_fn, void *call_site) {
  (void)call_site;
  ftrace_record((u64)(usize)this_fn, FTRACE_ENTER);
}

__attribute__((no_instrument_function)) void
__cyg_profile_func_exit(void *this_fn, void *call_site) {
  (void)call_site;
  ftrace_record((u64)(usize)this_fn, FTRACE_EXIT);
}
