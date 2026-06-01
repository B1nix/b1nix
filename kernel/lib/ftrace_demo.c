/* ftrace demo functions (M36). This translation unit is compiled WITH
 * -finstrument-functions (see the Makefile target-specific override), so each
 * function below emits __cyg_profile_func_enter/exit calls that the ftrace
 * engine records. The M36 self-test calls ftrace_demo_work() and checks the
 * trace captured these symbols. Kept tiny and side-effect-free. */

#include <b1nix/types.h>

/* Marked noinline so the instrumentation hooks actually fire per function
 * (an inlined leaf would not get its own enter/exit pair). */
__attribute__((noinline)) int ftrace_demo_leaf(int x) { return x * 2; }

__attribute__((noinline)) int ftrace_demo_work(int x) {
  return ftrace_demo_leaf(x) + 1;
}
