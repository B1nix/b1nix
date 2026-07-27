/* ftrace engine (M36). Holds the trace ring buffer and the GCC/clang
 * instrumentation hooks. This file is NOT compiled with -finstrument-functions
 * (so its own functions are never traced); the hooks are additionally marked
 * no_instrument_function for belt-and-braces. */

#include <b1nix/console.h>
#include <b1nix/ftrace.h>
#include <b1nix/klog.h>
#include <string.h>

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

/* ── in-memory self-test (M36 smoke, called from main.c in test mode) ── */

extern int ftrace_demo_work(int x);

void m36_ftrace_selftest(void) {
  console_write("M36-FTRACE: start\n");

  ftrace_reset();
  ftrace_enable();

  volatile int result = ftrace_demo_work(42);
  (void)result;

  ftrace_disable();

  usize count = ftrace_count();
  if (count < 4) {
    console_write("M36-FTRACE: FAIL capture (count=");
    char buf[16];
    int n = 0;
    usize v = count;
    if (v == 0) buf[n++] = '0';
    else {
      char tmp[16];
      int t = 0;
      while (v) { tmp[t++] = '0' + (v % 10); v /= 10; }
      while (t) buf[n++] = tmp[--t];
    }
    buf[n] = '\0';
    console_write(buf);
    console_write(")\n");
    return;
  }
  console_write("M36-FTRACE: ok capture\n");

  int found_work = 0, found_leaf = 0;
  for (usize i = 0; i < count; i++) {
    const struct ftrace_event *ev = ftrace_get(i);
    if (!ev) continue;
    u64 off = 0;
    const char *name = ksym_lookup(ev->addr, &off);
    if (!name) continue;
    if (strncmp(name, "ftrace_demo_work", 16) == 0 &&
        (name[16] == '\0' || name[16] == '+'))
      found_work = 1;
    if (strncmp(name, "ftrace_demo_leaf", 16) == 0 &&
        (name[16] == '\0' || name[16] == '+'))
      found_leaf = 1;
  }
  if (found_work || found_leaf)
    console_write("M36-FTRACE: ok symbolize\n");
  else
    console_write("M36-FTRACE: FAIL symbolize\n");

  console_write("M36-FTRACE: done\n");
}
