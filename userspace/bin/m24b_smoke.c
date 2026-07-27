/* M24b BKL proof: several CPU-bound userspace processes that sample which core
 * they run on. A SINGLE busy process is never migrated — it stays RUNNING on
 * the BSP, and an AP can only claim a task that is READY, so proving AP
 * execution needs real contention: fork WORKERS children, run them
 * concurrently, and take the highest CPU id any of them observed via
 * sched_getcpu(). The authoritative proof is the kernel's per-CPU syscall mask
 * (sched_user_cpu_mask), which getcpu() here exercises from ring 3 on whatever
 * core runs the process. */

#include <sched.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>

#define WORKERS 4

static void uwrite(const char *s) {
  size_t n = 0;
  while (s[n])
    n++;
  write(1, s, n);
}

/* Burn CPU while sampling the current core; returns the highest id seen. */
static int spin_and_sample(void) {
  int max_cpu = 0;
  volatile uint64_t acc = 0;
  for (int outer = 0; outer < 200; outer++) {
    for (volatile int inner = 0; inner < 40000; inner++)
      acc += (uint64_t)inner ^ (uint64_t)outer;

    int cpu = sched_getcpu();
    if (cpu > max_cpu)
      max_cpu = cpu;
  }
  (void)acc;
  return max_cpu;
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  pid_t pids[WORKERS];
  for (int i = 0; i < WORKERS; i++) {
    pids[i] = fork();
    if (pids[i] == 0) {
      /* Report the highest core through the exit status so the parent can
       * aggregate without a shared file. */
      _exit(spin_and_sample() & 0x7f);
    }
  }

  int max_cpu = spin_and_sample();
  for (int i = 0; i < WORKERS; i++) {
    if (pids[i] <= 0)
      continue;
    int st = 0;
    if (waitpid(pids[i], &st, 0) != pids[i])
      continue;
    if (WIFEXITED(st) && WEXITSTATUS(st) > max_cpu)
      max_cpu = WEXITSTATUS(st);
  }

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
