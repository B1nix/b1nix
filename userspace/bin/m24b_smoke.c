/* M24b BKL proof: a CPU-bound userspace process that samples which core it runs
 * on. The kernel spawns several copies concurrently (see init_main); the
 * cooperative scheduler distributes them across the BSP and Application
 * Processors under the Big Kernel Lock, so some land on APs. Each instance
 * reports the highest CPU id it observed via getcpu(); the authoritative proof
 * is the kernel's per-CPU syscall mask (sched_user_cpu_mask), which getcpu()
 * here exercises from ring 3 on whatever core runs this process. */

#include <sched.h>
#include <stdint.h>
#include <unistd.h>

static void uwrite(const char *s) {
  size_t n = 0;
  while (s[n])
    n++;
  write(1, s, n);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  int max_cpu = 0;

  volatile uint64_t acc = 0;
  for (int outer = 0; outer < 400; outer++) {
    for (volatile int inner = 0; inner < 40000; inner++)
      acc += (uint64_t)inner ^ (uint64_t)outer;

    int cpu = sched_getcpu();
    if (cpu > max_cpu)
      max_cpu = cpu;
  }
  (void)acc;

  char buf[32];
  const char *p = "M24B-BKL: instance cpu=";
  int i = 0;
  while (p[i]) {
    buf[i] = p[i];
    i++;
  }
  buf[i++] = (char)('0' + (max_cpu % 10));
  buf[i++] = '\n';
  write(1, buf, i);

  if (max_cpu > 0)
    uwrite("M24B-BKL: instance ran-on-ap\n");

  return 0;
}
