/* SPDX-License-Identifier: GPL-2.0-only */
/* m129_suspend_smoke — s2idle with a real wake source (M129).
 *
 * A suspend is easy to fake. `echo freeze > /sys/power/state` returning 0 is
 * worth nothing on its own: a kernel that ignored the write entirely would
 * pass that. What is worth something is the shape of a suspend, observed from
 * outside it:
 *
 *   - the write blocks for about as long as the alarm was armed for, so the
 *     machine really waited for the wake rather than returning at once;
 *   - a child process that spends its whole life spinning has a hole of that
 *     length in its OWN timeline, so userspace really stopped; and
 *   - the RTC alarm interrupt was really taken, which /proc/interrupts says.
 *
 * The child measures the hole itself, comparing consecutive CLOCK_MONOTONIC
 * readings and keeping the longest gap it ever sees. Having the parent read a
 * counter before and after would race the thaw — on an SMP machine the child
 * is running again before the write returns — while a gap the child recorded
 * with its own clock cannot be produced by anything except the child not
 * running.
 *
 * A baseline pass first: if this machine cannot keep the child scheduled
 * within a third of a second when nothing is suspended (a badly contended
 * host), the gap proves nothing and the marker is not printed rather than
 * printed on weaker evidence.
 *
 * Markers (only emitted on verified success):
 *   M129-SUSPEND: start
 *   M129-SUSPEND: ok state-lists-freeze
 *   M129-SUSPEND: ok unknown-state-refused
 *   M129-SUSPEND: ok freeze-without-alarm
 *   M129-SUSPEND: ok alarm-armed
 *   M129-SUSPEND: ok slept-the-interval
 *   M129-SUSPEND: ok child-frozen
 *   M129-SUSPEND: ok child-runs-again
 *   M129-SUSPEND: ok rtc-irq-counted
 *   M129-SUSPEND: ok machine-alive
 *   M129-SUSPEND: ok s3-slept        (only where the firmware declares \_S3)
 *   M129-SUSPEND: ok s3-alive
 *   M129-SUSPEND: ok s3-devices
 *   M129-SUSPEND: done
 */
#include <b1nix/drm.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* From <linux/rtc.h>, spelled out so this program does not depend on which
 * sysroot supplies it (m107_smoke does the same). */
#define RTC_AIE_OFF   0x7002
#define RTC_RD_TIME   0x80247009
#define RTC_WKALM_SET 0x4028700f

struct rtc_time_u {
  int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday,
      tm_isdst;
};
struct rtc_wkalrm_u {
  unsigned char enabled, pending, pad[2];
  struct rtc_time_u time;
};

/* How far ahead the alarm is armed. The CMOS alarm matches whole seconds, so
 * the real interval is somewhere in (ALARM_AHEAD-1, ALARM_AHEAD] seconds. */
#define ALARM_AHEAD 3

struct shared {
  volatile unsigned long count;    /* the child is alive and running */
  volatile unsigned long max_gap;  /* longest gap between iterations, ns */
  volatile unsigned long reset;    /* parent asks for max_gap to be cleared */
  volatile unsigned long reset_ack;
};

static int fails;

static void ok(const char *what) {
  printf("M129-SUSPEND: ok %s\n", what);
  fflush(stdout);
}

static void bad(const char *what, const char *why, long v) {
  printf("M129-SUSPEND: fail %s (%s, %ld)\n", what, why, v);
  fflush(stdout);
  fails++;
}

static void judge(const char *what, int good, const char *why, long v) {
  if (good)
    ok(what);
  else
    bad(what, why, v);
}

static unsigned long long now_ns(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long)ts.tv_sec * 1000000000ull +
         (unsigned long long)ts.tv_nsec;
}

static int read_file(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  ssize_t n;

  if (fd < 0)
    return -1;
  n = read(fd, buf, cap - 1);
  close(fd);
  if (n < 0)
    return -1;
  buf[n] = 0;
  return (int)n;
}

/* Write to /sys/power/state. Returns 0, or -errno. */
static int write_state(const char *s) {
  int fd = open("/sys/power/state", O_WRONLY);
  ssize_t n;
  int err;

  if (fd < 0)
    return -errno;
  n = write(fd, s, strlen(s));
  err = errno;
  close(fd);
  if (n < 0)
    return -err;
  return 0;
}

/* The RTC alarm row of /proc/interrupts, or -1 if there is not one. */
static long rtc_irq_count(void) {
  char buf[4096];
  const char *p;

  if (read_file("/proc/interrupts", buf, sizeof(buf)) < 0)
    return -1;
  p = strstr(buf, "RTC:");
  if (!p)
    return -1;
  return strtol(p + 4, 0, 10);
}

/* The counter is published LAST, after the gap this iteration measured. A
 * parent that has seen the counter move has therefore also seen the gap --
 * which is what makes "wait for one more iteration, then read max_gap" a
 * measurement rather than a race. */
static void child_spin(struct shared *sh) {
  unsigned long long prev = now_ns();
  unsigned long acked = 0;

  for (;;) {
    unsigned long long n;
    unsigned long long gap;

    n = now_ns();
    if (sh->reset != acked) {
      acked = sh->reset;
      sh->max_gap = 0;
      sh->reset_ack = acked;
      prev = n;
      sh->count++;
      continue;
    }
    gap = n > prev ? n - prev : 0;
    if (gap > sh->max_gap)
      sh->max_gap = (unsigned long)gap;
    prev = n;
    sh->count++;
  }
}

/* Wait until the child has completed `n` more iterations, so whatever it
 * measured in them has been published. Returns 0 on success. */
static int wait_iterations(struct shared *sh, unsigned long n,
                           unsigned long long budget_ns) {
  unsigned long target = sh->count + n;
  unsigned long long deadline = now_ns() + budget_ns;

  while (sh->count < target) {
    if (now_ns() > deadline)
      return -1;
    usleep(1000);
  }
  return 0;
}

/* Ask the child to forget the gaps it has seen so far, and wait until it
 * confirms — otherwise a gap from before the freeze is still in the record. */
static int reset_gap(struct shared *sh) {
  unsigned long want = sh->reset + 1;
  unsigned long long deadline = now_ns() + 2000000000ull;

  sh->reset = want;
  while (sh->reset_ack != want) {
    if (now_ns() > deadline)
      return -1;
    usleep(1000);
  }
  return 0;
}

/* The machine still works: a file round-trip, a fork that is waited for, and
 * a syscall whose answer we can check. */
static int machine_alive(void) {
  const char *path = "/tmp/m129-suspend-alive";
  char buf[32];
  int fd, st = 0;
  pid_t p;

  fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    return 0;
  if (write(fd, "resumed", 7) != 7) {
    close(fd);
    return 0;
  }
  if (lseek(fd, 0, SEEK_SET) != 0) {
    close(fd);
    return 0;
  }
  memset(buf, 0, sizeof(buf));
  if (read(fd, buf, sizeof(buf) - 1) != 7 || strcmp(buf, "resumed")) {
    close(fd);
    return 0;
  }
  close(fd);
  unlink(path);

  p = fork();
  if (p < 0)
    return 0;
  if (p == 0)
    _exit(41);
  if (waitpid(p, &st, 0) != p)
    return 0;
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 41)
    return 0;

  return getpid() > 0;
}

int main(void) {
  struct shared *sh;
  struct rtc_time_u t;
  struct rtc_wkalrm_u alarm;
  char buf[256];
  unsigned long long t0, t1, elapsed_ms;
  unsigned long baseline_gap, frozen_gap, count_before, count_after;
  long irq_before, irq_after;
  pid_t child;
  int rtc, rc, baseline_usable;

  printf("M129-SUSPEND: start\n");
  fflush(stdout);

  /* 1. What the machine says it supports. */
  if (read_file("/sys/power/state", buf, sizeof(buf)) < 0) {
    bad("state-lists-freeze", "no /sys/power/state", errno);
    printf("M129-SUSPEND: done\n");
    return 1;
  }
  judge("state-lists-freeze", strstr(buf, "freeze") != 0, "states", 0);

  /* 2. A state it does not have is refused, not accepted and ignored. */
  rc = write_state("bogus");
  judge("unknown-state-refused", rc == -EINVAL, "rc", rc);

  /* 3. With the alarm disarmed, a suspend still has a way out: this machine
   *    has a keyboard and a console, and a key is a wake source on every
   *    machine that has one — which is why `systemctl suspend`, which arms no
   *    alarm at all, must not be refused. What has to hold is that the
   *    machine comes back on its own and is still working, not that the
   *    kernel refuses. (Before there was an input wake source this asked for
   *    ENODEV; a kernel that refuses to suspend a laptop because nobody set
   *    an alarm is the wrong behaviour to lock in.) */
  rtc = open("/dev/rtc0", O_RDWR);
  if (rtc < 0)
    rtc = open("/dev/rtc", O_RDWR);
  if (rtc < 0) {
    bad("alarm-armed", "no /dev/rtc0", errno);
    printf("M129-SUSPEND: done\n");
    return 1;
  }
  if (ioctl(rtc, RTC_AIE_OFF, 0) != 0) {
    bad("freeze-without-alarm", "RTC_AIE_OFF", errno);
  } else {
    unsigned long long a = now_ns(), b;

    rc = write_state("freeze");
    b = now_ns();
    /* It returns, within the kernel's own ceiling, and the machine is alive
     * afterwards — which the checks below go on to use. */
    printf("M129-SUSPEND: freeze with no alarm returned after %llu ms\n",
           (b - a) / 1000000ull);
    fflush(stdout);
    judge("freeze-without-alarm", rc >= 0 && (b - a) < 11000000000ull,
          "rc/elapsed ms", rc < 0 ? rc : (long)((b - a) / 1000000ull));
  }

  /* 4. A child that does nothing but run, and measure that it is running. */
  sh = mmap(0, sizeof(*sh), PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (sh == MAP_FAILED) {
    bad("child-frozen", "mmap", errno);
    close(rtc);
    printf("M129-SUSPEND: done\n");
    return 1;
  }
  memset((void *)sh, 0, sizeof(*sh));
  child = fork();
  if (child < 0) {
    bad("child-frozen", "fork", errno);
    close(rtc);
    printf("M129-SUSPEND: done\n");
    return 1;
  }
  if (child == 0) {
    child_spin(sh);
    _exit(0);
  }

  printf("M129-SUSPEND: parent pid %d child pid %d\n", (int)getpid(),
         (int)child);
  fflush(stdout);

  /* 5. Baseline: how long does this machine leave the child unscheduled when
   *    nothing at all is suspended? */
  sleep(1);
  baseline_gap = sh->max_gap;
  count_before = sh->count;
  if (count_before == 0) {
    bad("child-frozen", "child never ran", 0);
    kill(child, SIGKILL);
    waitpid(child, 0, 0);
    close(rtc);
    printf("M129-SUSPEND: done\n");
    return 1;
  }
  baseline_usable = baseline_gap < 300000000ul; /* 300 ms */
  printf("M129-SUSPEND: baseline gap %lu ms over 1 s\n",
         baseline_gap / 1000000ul);
  fflush(stdout);
  if (reset_gap(sh) != 0) {
    bad("child-frozen", "child did not acknowledge the reset", 0);
    baseline_usable = 0;
  }

  /* 6. Arm the alarm a few seconds out, against the HARDWARE clock — that is
   *    what the alarm registers are compared with. */
  if (ioctl(rtc, RTC_RD_TIME, &t) != 0) {
    bad("alarm-armed", "RTC_RD_TIME", errno);
    kill(child, SIGKILL);
    waitpid(child, 0, 0);
    close(rtc);
    printf("M129-SUSPEND: done\n");
    return 1;
  }
  memset(&alarm, 0, sizeof(alarm));
  alarm.enabled = 1;
  alarm.time = t;
  alarm.time.tm_sec += ALARM_AHEAD;
  if (alarm.time.tm_sec >= 60) {
    alarm.time.tm_sec -= 60;
    alarm.time.tm_min++;
  }
  if (alarm.time.tm_min >= 60) {
    alarm.time.tm_min -= 60;
    alarm.time.tm_hour++;
  }
  if (alarm.time.tm_hour >= 24)
    alarm.time.tm_hour -= 24;
  rc = ioctl(rtc, RTC_WKALM_SET, &alarm);
  judge("alarm-armed", rc == 0, "RTC_WKALM_SET", rc == 0 ? 0 : errno);
  if (rc != 0) {
    kill(child, SIGKILL);
    waitpid(child, 0, 0);
    close(rtc);
    printf("M129-SUSPEND: done\n");
    return 1;
  }

  irq_before = rtc_irq_count();
  if (irq_before < 0)
    irq_before = 0; /* the row only appears once the line has fired */

  /* 7. Suspend. The write returns when the machine has resumed. */
  t0 = now_ns();
  rc = write_state("freeze");
  t1 = now_ns();
  elapsed_ms = (t1 - t0) / 1000000ull;
  count_after = sh->count;
  irq_after = rtc_irq_count();
  /* The thaw happens inside the write, so on a machine with a spare CPU the
   * child may not have run yet when the write returns -- and the gap it is
   * about to record is the whole measurement. Wait for it. max_gap only ever
   * grows, so a later read cannot lose what an earlier one would have seen. */
  if (wait_iterations(sh, 2, 5000000000ull) != 0)
    bad("child-runs-again", "the child never ran again", 0);
  frozen_gap = sh->max_gap;

  printf("M129-SUSPEND: write rc %d, %llu ms elapsed, child gap %lu ms\n", rc,
         elapsed_ms, frozen_gap / 1000000ul);
  fflush(stdout);

  if (rc != 0) {
    bad("slept-the-interval", "write failed", rc);
  } else {
    /* The alarm matches a whole second, so the true interval is at least
     * ALARM_AHEAD-1 seconds; anything near the kernel's 30 s ceiling means
     * the alarm never fired and the safety bound resumed us. */
    judge("slept-the-interval",
          elapsed_ms >= (unsigned long long)(ALARM_AHEAD - 1) * 1000ull &&
              elapsed_ms < 20000ull,
          "ms", (long)elapsed_ms);
  }

  /* 8. The child stopped — as witnessed by the child's own clock. */
  if (!baseline_usable)
    printf("M129-SUSPEND: skip child-frozen (baseline gap %lu ms is already "
           "too large to prove anything)\n",
           baseline_gap / 1000000ul);
  else
    judge("child-frozen",
          rc == 0 && frozen_gap >= 1000000000ul &&
              frozen_gap >= (unsigned long)(elapsed_ms * 1000000ull / 2),
          "gap ms", (long)(frozen_gap / 1000000ul));

  /* 9. And it is running again — not merely one iteration further, but
   *    counting at its old rate. */
  {
    unsigned long c0 = sh->count;

    usleep(200000);
    judge("child-runs-again", sh->count > c0 + 1000 && count_after >= count_before,
          "iterations in 200 ms", (long)(sh->count - c0));
  }

  /* 10. The interrupt was really taken. */
  judge("rtc-irq-counted", irq_after > irq_before, "count", irq_after);

  /* 11. The machine still works. */
  judge("machine-alive", machine_alive(), "post-resume", 0);

  /* 12. ACPI S3, where the machine has it.
   *
   * This is a different thing from the freeze above, and the difference is the
   * point: the processor's state is GONE — the firmware powers it down and the
   * kernel comes back through a real-mode trampoline — so a suspend that
   * resumed and a machine that merely idled cannot be confused. What proves it
   * is the kernel's own S3 counter, which an s2idle suspend never moves, and
   * the machine still working afterwards. */
  {
    char states[64] = "";
    char proc[512] = "";

    read_file("/sys/power/state", states, sizeof(states));
    if (!strstr(states, "mem")) {
      read_file("/proc/b1nix-suspend", proc, sizeof(proc));
      {
        char *why = strstr(proc, "s3_absent ");

        printf("M129-SUSPEND: no-s3 %.*s\n",
               why ? (int)strcspn(why + 10, "\n") : 7,
               why ? why + 10 : "unknown");
      }
      fflush(stdout);
    } else {
      long count_before_s3 = -1, count_after_s3 = -1, last_ms = -1;
      unsigned long long s3_elapsed;
      char *p;

      read_file("/proc/b1nix-suspend", proc, sizeof(proc));
      p = strstr(proc, "s3_count ");
      if (p)
        count_before_s3 = strtol(p + 9, 0, 10);

      /* Arm the alarm again: the sleep that just happened consumed it. */
      if (ioctl(rtc, RTC_RD_TIME, &t) == 0) {
        memset(&alarm, 0, sizeof(alarm));
        alarm.enabled = 1;
        alarm.time = t;
        alarm.time.tm_sec += ALARM_AHEAD;
        if (alarm.time.tm_sec >= 60) {
          alarm.time.tm_sec -= 60;
          alarm.time.tm_min++;
        }
        if (alarm.time.tm_min >= 60) {
          alarm.time.tm_min -= 60;
          alarm.time.tm_hour++;
        }
        if (alarm.time.tm_hour >= 24)
          alarm.time.tm_hour -= 24;
        rc = ioctl(rtc, RTC_WKALM_SET, &alarm);
      } else {
        rc = -1;
      }
      if (rc != 0) {
        bad("s3-slept", "the alarm could not be armed for the S3 test", errno);
      } else {
        t0 = now_ns();
        rc = write_state("mem");
        t1 = now_ns();
        s3_elapsed = (t1 - t0) / 1000000ull;
        read_file("/proc/b1nix-suspend", proc, sizeof(proc));
        p = strstr(proc, "s3_count ");
        if (p)
          count_after_s3 = strtol(p + 9, 0, 10);
        p = strstr(proc, "s3_last_ms ");
        if (p)
          last_ms = strtol(p + 11, 0, 10);
        printf("M129-SUSPEND: s3 write rc %d, %llu ms elapsed, kernel says "
               "%ld ms, count %ld -> %ld\n",
               rc, s3_elapsed, last_ms, count_before_s3, count_after_s3);
        fflush(stdout);
        /* The counter is the witness: it moves only on the path that wrote
         * SLP_TYP into PM1_CNT and came back through the wake-up trampoline. */
        judge("s3-slept",
              rc == 0 && count_after_s3 == count_before_s3 + 1 &&
                  s3_elapsed >= (unsigned long long)(ALARM_AHEAD - 1) * 1000ull &&
                  s3_elapsed < 20000ull,
              "ms", (long)s3_elapsed);
        /* And the machine is a machine again: the processor was rebuilt from
         * what the kernel saved, so a file round trip, a fork and a syscall all
         * have to work. */
        judge("s3-alive", machine_alive(), "post-S3", 0);

        /* And the DEVICES, which is the half of a suspend that is easy to
         * get wrong quietly: the machine can be perfectly alive with a disk
         * that answers nothing, a display that never updates again and an
         * input device that reports no keys. Each of these is opened and
         * exercised as far as a test without a human at the keyboard can:
         * a non-blocking read of an evdev node must say "nothing yet"
         * (EAGAIN) rather than "no such device", the DRM node must still
         * answer an ioctl, and the audio device must still take a buffer. */
        {
          int good = 1;
          int step = 0;
          int fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
          char ev[32];

          if (fd < 0) {
            good = 0;
            step = 1;
          } else {
            ssize_t n = read(fd, ev, sizeof(ev));

            if (n < 0 && errno != EAGAIN) {
              good = 0;
              step = 2;
            }
            close(fd);
          }
          if (good) {
            fd = open("/dev/dri/card0", O_RDWR);
            if (fd < 0) {
              good = 0;
              step = 3;
            } else {
              /* Not just a query: a frame, all the way to the device.
               *
               * GETRESOURCES and GETCONNECTOR are answered out of the driver's
               * own memory and would pass on a card whose queues never came
               * back. CREATE_DUMB, ADDFB and SETCRTC are not: the last of them
               * sends the scanout command to the device, so this fails on a
               * virtio-gpu whose virtqueues were left where the reset put them.
               */
              uint32_t crtc = 0, connector = 0;
              struct drm_mode_card_res res;

              memset(&res, 0, sizeof(res));
              res.crtc_id_ptr = (uint64_t)(uintptr_t)&crtc;
              res.connector_id_ptr = (uint64_t)(uintptr_t)&connector;
              res.count_crtcs = 1;
              res.count_connectors = 1;
              if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0 || !crtc ||
                  !connector) {
                good = 0;
                step = 4;
              } else {
                struct drm_mode_modeinfo mode;
                struct drm_mode_get_connector conn;

                memset(&mode, 0, sizeof(mode));
                memset(&conn, 0, sizeof(conn));
                conn.connector_id = connector;
                conn.modes_ptr = (uint64_t)(uintptr_t)&mode;
                conn.count_modes = 1;
                if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) != 0 ||
                    !mode.hdisplay) {
                  good = 0;
                  step = 5;
                } else {
                  struct drm_mode_create_dumb d;
                  struct drm_mode_fb_cmd fbc;
                  struct drm_mode_crtc set;

                  memset(&d, 0, sizeof(d));
                  d.width = mode.hdisplay;
                  d.height = mode.vdisplay;
                  d.bpp = 32;
                  memset(&fbc, 0, sizeof(fbc));
                  memset(&set, 0, sizeof(set));
                  if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &d) != 0) {
                    good = 0;
                    step = 6;
                  } else {
                    fbc.width = d.width;
                    fbc.height = d.height;
                    fbc.pitch = d.pitch;
                    fbc.bpp = 32;
                    fbc.depth = 24;
                    fbc.handle = d.handle;
                    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbc) != 0) {
                      good = 0;
                      step = 7;
                    } else {
                      set.crtc_id = crtc;
                      set.fb_id = fbc.fb_id;
                      set.set_connectors_ptr = (uint64_t)(uintptr_t)&connector;
                      set.count_connectors = 1;
                      set.mode = mode;
                      set.mode_valid = 1;
                      if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) != 0) {
                        good = 0;
                        step = 8;
                      }
                    }
                  }
                }
              }
              close(fd);
            }
          }
          if (good) {
            /* A tone, not silence, and at 880 Hz — a frequency nothing else in
             * this lane plays (the audio test's own tone is 440). The lane's
             * capture file is checked for it afterwards, so this proves the
             * samples reached the emulated card AFTER the sleep rather than
             * that a write returned a byte count. */
            static short tone[48000];   /* half a second, stereo, 48 kHz */
            const int frames = 12000;

            for (int i = 0; i < frames; i++) {
              double t = (double)i / 48000.0;
              short v = (short)(12000.0 * sin(2.0 * 3.14159265358979 * 880.0 * t));

              tone[i * 2] = v;
              tone[i * 2 + 1] = v;
            }
            fd = open("/dev/dsp1", O_WRONLY);
            if (fd < 0) {
              /* A machine with no audio device is not a failure here. */
              printf("M129-SUSPEND: no audio device after the resume\n");
              fflush(stdout);
            } else {
              ssize_t wr = write(fd, (const char *)tone,
                                 (size_t)frames * 2 * sizeof(short));

              printf("M129-SUSPEND: audio 880Hz wrote %ld bytes\n", (long)wr);
              fflush(stdout);
              if (wr <= 0) {
                good = 0;
                step = 9;
              }
              /* Let the DMA run before the machine goes on to other things. */
              usleep(600000);
              close(fd);
            }
          }
          judge("s3-devices", good, "step", (long)step);
        }
      }
    }
  }

  ioctl(rtc, RTC_AIE_OFF, 0);
  close(rtc);
  kill(child, SIGKILL);
  waitpid(child, 0, 0);
  munmap((void *)sh, sizeof(*sh));

  printf("M129-SUSPEND: done\n");
  fflush(stdout);
  return fails ? 1 : 0;
}
