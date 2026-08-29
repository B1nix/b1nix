/* /dev/rtc0 — the CMOS real-time clock as a character device (M107).
 *
 * kernel/arch/x86_64/rtc.c reads the CMOS once at boot and from then on the
 * wall clock is "boot snapshot + uptime + software offset". `hwclock` needs
 * the other half: read the hardware clock *now*, and write it back. This file
 * is that half — real CMOS reads and writes, including the century register
 * and the BCD/12-hour encodings, plus the alarm registers `rtcwake` sets.
 */

#include <b1nix/ktime.h>
#include <b1nix/errno.h>
#include <b1nix/io.h>
#include <b1nix/posix.h>
#include <b1nix/resource_caps.h>
#include <b1nix/rtc.h>
#include <b1nix/uidgid.h>
#include <stdio.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <b1nix/vfs.h>
#include <string.h>
#include <b1nix/platform.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

/* CMOS register numbers. */
#define CMOS_SEC      0x00
#define CMOS_ALRM_SEC 0x01
#define CMOS_MIN      0x02
#define CMOS_ALRM_MIN 0x03
#define CMOS_HOUR     0x04
#define CMOS_ALRM_HR  0x05
#define CMOS_WDAY     0x06
#define CMOS_MDAY     0x07
#define CMOS_MONTH    0x08
#define CMOS_YEAR     0x09
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B
#define CMOS_CENTURY  0x32

#define RTC_B_DM  0x04 /* set: registers are binary, clear: BCD */
#define RTC_B_24H 0x02 /* set: 24-hour, clear: 12-hour with a PM bit */
#define RTC_B_AIE 0x20 /* alarm interrupt enable */
#define RTC_B_UIE 0x10 /* update-ended interrupt enable */
#define RTC_B_PIE 0x40 /* periodic interrupt enable */
#define RTC_B_SET 0x80 /* inhibit updates while we write */

/* ioctl numbers from Linux <linux/rtc.h>. struct rtc_time is 9 ints (36
 * bytes); struct rtc_wkalrm is 40 with its two leading bytes and padding. */
#define RTC_AIE_ON    0x7001
#define RTC_AIE_OFF   0x7002
#define RTC_UIE_ON    0x7003
#define RTC_UIE_OFF   0x7004
#define RTC_PIE_ON    0x7005
#define RTC_PIE_OFF   0x7006
#define RTC_ALM_SET   0x40247007
#define RTC_ALM_READ  0x80247008
#define RTC_RD_TIME   0x80247009
#define RTC_SET_TIME  0x4024700a
#define RTC_IRQP_READ 0x8008700b
#define RTC_IRQP_SET  0x4008700c
#define RTC_WKALM_SET 0x4028700f
#define RTC_WKALM_RD  0x80287010

struct rtc_time_k {
  int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
  int tm_wday, tm_yday, tm_isdst;
};

struct rtc_wkalrm_k {
  u8 enabled;
  u8 pending;
  u8 pad[2];
  struct rtc_time_k time;
};

static spinlock_t rtc_lock = SPINLOCK_INIT;
static u32 g_irq_freq = 2;

/* Calendar helpers, shared by both backends. */
static int is_leap(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int days_in_month(int year, int mon /* 0-11 */) {
  static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (mon == 1 && is_leap(year))
    return 29;
  return d[mon];
}

/* Read the hardware clock. Retries until two consecutive reads agree and the
 * update-in-progress flag is clear, which is the only way to avoid a torn
 * value across a second boundary. */

#if defined(__aarch64__)
/* ── PL031 backend (QEMU virt) ───────────────────────────────────────────
 * The ARM PrimeCell RTC is a plain 32-bit seconds-since-epoch counter with one
 * match register for the alarm — no BCD, no update-in-progress window, and no
 * periodic interrupt. Everything below the node/ioctl layer is therefore much
 * simpler than the CMOS backend it replaces on this arch. QEMU maps it at
 * 0x09010000, inside the identity-mapped low MMIO window the GIC and UART also
 * live in, so no mapping call is needed. */
#define PL031_BASE 0x09010000ULL
#define PL031_DR   0x000 /* data: current value, seconds */
#define PL031_MR   0x004 /* match: alarm */
#define PL031_LR   0x008 /* load: set the counter */
#define PL031_CR   0x00c /* control: bit 0 enables */
#define PL031_IMSC 0x010 /* interrupt mask set/clear */
#define PL031_RIS  0x014 /* raw interrupt status */
#define PL031_ICR  0x01c /* interrupt clear */

/* QEMU virt has a PL031 at this address. Nothing else does.
 *
 * A Raspberry Pi has no battery-backed clock at all, and on a Snapdragon 855
 * 0x09010000 is not an RTC — it is an unclaimed window in the Qualcomm
 * peripheral space, where a load does not return a wrong value, it does not
 * return. vfs_get_unix_time() calls rtc_now_unix_seconds() on every VFS node
 * creation, so on that board this single hardcoded read wedged the boot inside
 * the first vfs_add_node() after the initramfs, with no output and no fault. */
static int pl031_present(void) {
  return platform_type() == PLATFORM_QEMU_VIRT;
}

/* Registers on a board that has the device; a scratch word per offset on one
 * that does not, so every caller below keeps working and nothing reaches a bus
 * that will not answer. */
static u32 pl031_absent_regs[8];
static u64 g_rtc_soft_seconds; /* the settable clock a board with no RTC gets */

static volatile u32 *pl031_reg(u32 off) {
  if (!pl031_present())
    return &pl031_absent_regs[(off / 4) & 7];
  return (volatile u32 *)(usize)(PL031_BASE + off);
}

/* Days since the epoch for a Gregorian date, then the usual 86400 scaling. */
static u64 rtc_tm_to_epoch(const struct rtc_time_k *t) {
  int year = t->tm_year + 1900;
  i64 days = 0;
  for (int y = 1970; y < year; y++)
    days += is_leap(y) ? 366 : 365;
  for (int m = 0; m < t->tm_mon; m++)
    days += days_in_month(year, m);
  days += t->tm_mday - 1;
  return (u64)days * 86400ULL + (u64)t->tm_hour * 3600ULL +
         (u64)t->tm_min * 60ULL + (u64)t->tm_sec;
}

static void rtc_epoch_to_tm(u64 secs, struct rtc_time_k *out) {
  memset(out, 0, sizeof(*out));
  u64 days = secs / 86400ULL;
  u32 rem = (u32)(secs % 86400ULL);
  out->tm_hour = (int)(rem / 3600);
  out->tm_min = (int)((rem % 3600) / 60);
  out->tm_sec = (int)(rem % 60);
  out->tm_wday = (int)((days + 4) % 7); /* 1970-01-01 was a Thursday */
  int year = 1970;
  while (1) {
    u64 ylen = is_leap(year) ? 366 : 365;
    if (days < ylen)
      break;
    days -= ylen;
    year++;
  }
  out->tm_year = year - 1900;
  out->tm_yday = (int)days;
  int mon = 0;
  while (mon < 11 && days >= (u64)days_in_month(year, mon)) {
    days -= (u64)days_in_month(year, mon);
    mon++;
  }
  out->tm_mon = mon;
  out->tm_mday = (int)days + 1;
  out->tm_isdst = -1;
}

static int rtc_hw_read(struct rtc_time_k *out) {
  rtc_epoch_to_tm(rtc_now_unix_seconds(), out);
  return 0;
}

static int rtc_hw_write(const struct rtc_time_k *t) {
  int year = t->tm_year + 1900;
  if (t->tm_mon < 0 || t->tm_mon > 11 || t->tm_mday < 1 ||
      t->tm_mday > days_in_month(year, t->tm_mon) || t->tm_hour < 0 ||
      t->tm_hour > 23 || t->tm_min < 0 || t->tm_min > 59 || t->tm_sec < 0 ||
      t->tm_sec > 59 || year < 1970 || year > 3000)
    return -EINVAL;
  u64 flags;
  spin_lock_irqsave(&rtc_lock, &flags);
  *pl031_reg(PL031_LR) = (u32)rtc_tm_to_epoch(t);
  *pl031_reg(PL031_CR) = 1;
  spin_unlock_irqrestore(&rtc_lock, flags);
  return 0;
}

static int rtc_alarm_read(struct rtc_wkalrm_k *out) {
  u64 flags;
  spin_lock_irqsave(&rtc_lock, &flags);
  u32 match = *pl031_reg(PL031_MR);
  u32 mask = *pl031_reg(PL031_IMSC);
  u32 ris = *pl031_reg(PL031_RIS);
  spin_unlock_irqrestore(&rtc_lock, flags);
  memset(out, 0, sizeof(*out));
  rtc_epoch_to_tm(match, &out->time);
  out->enabled = (mask & 1) ? 1 : 0;
  out->pending = (ris & 1) ? 1 : 0;
  return 0;
}

/* The alarm register holds a full timestamp, but RTC_ALM_SET only carries
 * hour/min/sec — the same "next time those hands come round" semantics the
 * CMOS backend has. Build it from today's date and roll to tomorrow when the
 * time of day has already passed. */
static int rtc_alarm_write(const struct rtc_time_k *t, int enable) {
  if (t->tm_sec < 0 || t->tm_sec > 59 || t->tm_min < 0 || t->tm_min > 59 ||
      t->tm_hour < 0 || t->tm_hour > 23)
    return -EINVAL;
  u64 flags;
  spin_lock_irqsave(&rtc_lock, &flags);
  u64 now = *pl031_reg(PL031_DR);
  u64 midnight = (now / 86400ULL) * 86400ULL;
  u64 when = midnight + (u64)t->tm_hour * 3600ULL + (u64)t->tm_min * 60ULL +
             (u64)t->tm_sec;
  if (when <= now)
    when += 86400ULL;
  *pl031_reg(PL031_MR) = (u32)when;
  *pl031_reg(PL031_ICR) = 1;
  *pl031_reg(PL031_IMSC) = enable ? 1u : 0u;
  spin_unlock_irqrestore(&rtc_lock, flags);
  return 0;
}

/* Only the alarm interrupt exists on PL031; there is no update-done or
 * periodic source to switch on, so those are reported as unsupported rather
 * than silently accepted. */
static int rtc_set_status_b(u8 bit, int on) {
  if (bit != RTC_B_AIE)
    return -EINVAL;
  u64 flags;
  spin_lock_irqsave(&rtc_lock, &flags);
  *pl031_reg(PL031_IMSC) = on ? 1u : 0u;
  spin_unlock_irqrestore(&rtc_lock, flags);
  return 0;
}

u64 rtc_now_unix_seconds(void) {
  if (!pl031_present())
    return g_rtc_soft_seconds;
  return *pl031_reg(PL031_DR);
}

/* The wall clock in nanoseconds, with a sub-second part that actually moves.
 *
 * This used to be the seconds reading multiplied by a billion, so every
 * timestamp on this arch had nanoseconds of exactly zero. Two changes inside
 * one second were then indistinguishable, which is precisely what a directory
 * mtime is asked to distinguish: anything that re-reads a directory only when
 * its mtime moved (make, ccache, systemd's unit cache, package managers) was
 * blind to a second write in the same second.
 *
 * Built the way x86_64 builds it: ONE monotonic source carries the sub-second
 * part, anchored to the seconds the RTC read at boot, so the composite never
 * walks backwards. The RTC is re-read only when it ticks a new second, which
 * keeps long-run drift bounded to the hardware's own. */
u64 rtc_now_unix_nanos(void) {
  static u64 anchor_sec;      /* RTC seconds at the last resync */
  static u64 anchor_mono_ns;  /* monotonic reading at that moment */
  static int anchored;

  u64 sec = rtc_now_unix_seconds();
  u64 mono = ktime_monotonic_ns();

  if (!anchored || sec != anchor_sec) {
    /* First call, or the RTC moved on: re-anchor so the sub-second part
     * restarts from zero exactly when the second changes. */
    anchor_sec = sec;
    anchor_mono_ns = mono;
    anchored = 1;
  }

  u64 sub = mono - anchor_mono_ns;
  if (sub > 999999999ull)
    sub = 999999999ull; /* the RTC is late; hold at the end of the second */
  return sec * 1000000000ull + sub;
}

void rtc_set_unix_time(u64 sec) {
  u64 flags;
  spin_lock_irqsave(&rtc_lock, &flags);
  if (pl031_present()) {
    *pl031_reg(PL031_LR) = (u32)sec;
    *pl031_reg(PL031_CR) = 1;
  } else {
    g_rtc_soft_seconds = sec;
  }
  spin_unlock_irqrestore(&rtc_lock, flags);
}

#else
static u8 cmos_read(u8 reg) {
  outb(CMOS_ADDR, reg);
  return inb(CMOS_DATA);
}

static void cmos_write(u8 reg, u8 val) {
  outb(CMOS_ADDR, reg);
  outb(CMOS_DATA, val);
}

static int cmos_updating(void) { return cmos_read(CMOS_STATUS_A) & 0x80; }

static int bcd_to_bin(u8 v) { return (v & 0x0F) + ((v >> 4) * 10); }
static u8 bin_to_bcd(int v) { return (u8)(((v / 10) << 4) | (v % 10)); }

/* Days in a month, honouring the proleptic Gregorian leap rule. */
static int rtc_hw_read(struct rtc_time_k *out) {
  u8 s1[7], s2[7];
  u8 b;
  int tries;
  u64 flags;

  spin_lock_irqsave(&rtc_lock, &flags);
  for (tries = 0; tries < 1000000 && cmos_updating(); tries++)
    ;
  s1[0] = cmos_read(CMOS_SEC);
  s1[1] = cmos_read(CMOS_MIN);
  s1[2] = cmos_read(CMOS_HOUR);
  s1[3] = cmos_read(CMOS_MDAY);
  s1[4] = cmos_read(CMOS_MONTH);
  s1[5] = cmos_read(CMOS_YEAR);
  s1[6] = cmos_read(CMOS_WDAY);
  u8 century = cmos_read(CMOS_CENTURY);
  for (tries = 0; tries < 16; tries++) {
    /* Bounded, like rtc_init's: a wedged southbridge must never hang a
     * caller that is holding a spinlock with interrupts off. */
    for (int spin = 0; spin < 1000000 && cmos_updating(); spin++)
      ;
    s2[0] = cmos_read(CMOS_SEC);
    s2[1] = cmos_read(CMOS_MIN);
    s2[2] = cmos_read(CMOS_HOUR);
    s2[3] = cmos_read(CMOS_MDAY);
    s2[4] = cmos_read(CMOS_MONTH);
    s2[5] = cmos_read(CMOS_YEAR);
    s2[6] = cmos_read(CMOS_WDAY);
    if (memcmp(s1, s2, sizeof(s1)) == 0)
      break;
    memcpy(s1, s2, sizeof(s1));
  }
  b = cmos_read(CMOS_STATUS_B);
  spin_unlock_irqrestore(&rtc_lock, flags);

  int sec, min, hour, mday, mon, year, wday, cent;
  int pm = 0;
  if (!(b & RTC_B_24H) && (s1[2] & 0x80)) {
    pm = 1;
    s1[2] &= 0x7F;
  }
  if (b & RTC_B_DM) {
    sec = s1[0];
    min = s1[1];
    hour = s1[2];
    mday = s1[3];
    mon = s1[4];
    year = s1[5];
    wday = s1[6];
    cent = century;
  } else {
    sec = bcd_to_bin(s1[0]);
    min = bcd_to_bin(s1[1]);
    hour = bcd_to_bin(s1[2]);
    mday = bcd_to_bin(s1[3]);
    mon = bcd_to_bin(s1[4]);
    year = bcd_to_bin(s1[5]);
    wday = bcd_to_bin(s1[6]);
    cent = bcd_to_bin(century);
  }
  if (!(b & RTC_B_24H)) {
    if (pm && hour < 12)
      hour += 12;
    else if (!pm && hour == 12)
      hour = 0;
  }
  /* The century register is optional; a machine without one reports 0 or 0xff
   * and the 20xx assumption is the only thing left. */
  int full_year = (cent >= 19 && cent <= 30) ? cent * 100 + year : 2000 + year;

  if (mon < 1 || mon > 12 || mday < 1 || mday > 31 || hour > 23 || min > 59 ||
      sec > 60)
    return -EIO;

  memset(out, 0, sizeof(*out));
  out->tm_sec = sec;
  out->tm_min = min;
  out->tm_hour = hour;
  out->tm_mday = mday;
  out->tm_mon = mon - 1;
  out->tm_year = full_year - 1900;
  out->tm_wday = (wday >= 1 && wday <= 7) ? wday - 1 : 0;
  int yday = 0;
  for (int m = 0; m < out->tm_mon; m++)
    yday += days_in_month(full_year, m);
  out->tm_yday = yday + mday - 1;
  out->tm_isdst = -1;
  return 0;
}

static int rtc_hw_write(const struct rtc_time_k *t) {
  int year = t->tm_year + 1900;
  if (t->tm_mon < 0 || t->tm_mon > 11 || t->tm_mday < 1 ||
      t->tm_mday > days_in_month(year, t->tm_mon) || t->tm_hour < 0 ||
      t->tm_hour > 23 || t->tm_min < 0 || t->tm_min > 59 || t->tm_sec < 0 ||
      t->tm_sec > 59 || year < 1900 || year > 3000)
    return -EINVAL;

  u64 flags;
  spin_lock_irqsave(&rtc_lock, &flags);
  u8 b = cmos_read(CMOS_STATUS_B);
  /* Halt the update cycle for the duration of the write, then restore the
   * original control bits — this is the documented CMOS write sequence. */
  cmos_write(CMOS_STATUS_B, (u8)(b | RTC_B_SET));
  int binary = (b & RTC_B_DM) != 0;
  int h24 = (b & RTC_B_24H) != 0;

  int hour = t->tm_hour;
  u8 hour_raw;
  if (h24) {
    hour_raw = binary ? (u8)hour : bin_to_bcd(hour);
  } else {
    int pm = hour >= 12;
    int h12 = hour % 12;
    if (h12 == 0)
      h12 = 12;
    hour_raw = binary ? (u8)h12 : bin_to_bcd(h12);
    if (pm)
      hour_raw |= 0x80;
  }
  cmos_write(CMOS_SEC, binary ? (u8)t->tm_sec : bin_to_bcd(t->tm_sec));
  cmos_write(CMOS_MIN, binary ? (u8)t->tm_min : bin_to_bcd(t->tm_min));
  cmos_write(CMOS_HOUR, hour_raw);
  cmos_write(CMOS_MDAY, binary ? (u8)t->tm_mday : bin_to_bcd(t->tm_mday));
  cmos_write(CMOS_MONTH,
             binary ? (u8)(t->tm_mon + 1) : bin_to_bcd(t->tm_mon + 1));
  cmos_write(CMOS_YEAR, binary ? (u8)(year % 100) : bin_to_bcd(year % 100));
  cmos_write(CMOS_CENTURY,
             binary ? (u8)(year / 100) : bin_to_bcd(year / 100));
  cmos_write(CMOS_STATUS_B, b);
  spin_unlock_irqrestore(&rtc_lock, flags);
  return 0;
}

static int rtc_alarm_read(struct rtc_wkalrm_k *out) {
  u64 flags;
  spin_lock_irqsave(&rtc_lock, &flags);
  u8 b = cmos_read(CMOS_STATUS_B);
  u8 s = cmos_read(CMOS_ALRM_SEC);
  u8 m = cmos_read(CMOS_ALRM_MIN);
  u8 h = cmos_read(CMOS_ALRM_HR);
  u8 c = cmos_read(0x0C); /* status C: reading it clears the pending flags */
  spin_unlock_irqrestore(&rtc_lock, flags);

  int binary = (b & RTC_B_DM) != 0;
  memset(out, 0, sizeof(*out));
  out->enabled = (b & RTC_B_AIE) ? 1 : 0;
  out->pending = (c & 0x20) ? 1 : 0;
  out->time.tm_sec = binary ? s : bcd_to_bin(s);
  out->time.tm_min = binary ? m : bcd_to_bin(m);
  out->time.tm_hour = binary ? (h & 0x7F) : bcd_to_bin((u8)(h & 0x7F));
  out->time.tm_mday = -1;
  out->time.tm_mon = -1;
  out->time.tm_year = -1;
  out->time.tm_wday = -1;
  out->time.tm_yday = -1;
  out->time.tm_isdst = -1;
  return 0;
}

static int rtc_alarm_write(const struct rtc_time_k *t, int enable) {
  if (t->tm_sec < 0 || t->tm_sec > 59 || t->tm_min < 0 || t->tm_min > 59 ||
      t->tm_hour < 0 || t->tm_hour > 23)
    return -EINVAL;
  u64 flags;
  spin_lock_irqsave(&rtc_lock, &flags);
  u8 b = cmos_read(CMOS_STATUS_B);
  int binary = (b & RTC_B_DM) != 0;
  cmos_write(CMOS_ALRM_SEC, binary ? (u8)t->tm_sec : bin_to_bcd(t->tm_sec));
  cmos_write(CMOS_ALRM_MIN, binary ? (u8)t->tm_min : bin_to_bcd(t->tm_min));
  cmos_write(CMOS_ALRM_HR, binary ? (u8)t->tm_hour : bin_to_bcd(t->tm_hour));
  if (enable)
    b |= RTC_B_AIE;
  else
    b &= (u8)~RTC_B_AIE;
  cmos_write(CMOS_STATUS_B, b);
  spin_unlock_irqrestore(&rtc_lock, flags);
  return 0;
}

static int rtc_set_status_b(u8 bit, int on) {
  u64 flags;
  spin_lock_irqsave(&rtc_lock, &flags);
  u8 b = cmos_read(CMOS_STATUS_B);
  if (on)
    b |= bit;
  else
    b &= (u8)~bit;
  cmos_write(CMOS_STATUS_B, b);
  spin_unlock_irqrestore(&rtc_lock, flags);
  return 0;
}

#endif /* CMOS vs PL031 backend */

/* Reading /dev/rtc0 blocks for an interrupt in Linux; there is no RTC IRQ
 * consumer here, so a read reports the current time as text instead of lying
 * about an interrupt that will never arrive. */
static isize rtc_node_read(struct vfs_node *node, u64 offset, char *buf,
                           usize size, int flags) {
  (void)node;
  (void)flags;
  struct rtc_time_k t;
  if (rtc_hw_read(&t) < 0)
    return -EIO;
  char line[80];
  int n = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d\n",
                   t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                   t.tm_min, t.tm_sec);
  if (n < 0)
    return -EIO;
  usize len = (usize)n;
  if (offset >= len)
    return 0;
  len -= (usize)offset;
  if (len > size)
    len = size;
  memcpy(buf, line + offset, len);
  return (isize)len;
}

static int rtc_ioctl(struct vfs_node *node, u64 request, void *arg) {
  (void)node;
  switch (request) {
  case RTC_RD_TIME: {
    struct rtc_time_k t;
    int rc = rtc_hw_read(&t);
    if (rc < 0)
      return rc;
    if (!arg || syscall_copyout(arg, &t, sizeof(t)) < 0)
      return -EFAULT;
    return 0;
  }
  case RTC_SET_TIME: {
    const struct cred *cred = scheduler_get_current_cred();
    if (cred && cred->euid != ROOT_UID && !cred_has_cap(cred, CAP_SYS_TIME))
      return -EPERM;
    struct rtc_time_k t;
    if (!arg || syscall_copyin(&t, arg, sizeof(t)) < 0)
      return -EFAULT;
    return rtc_hw_write(&t);
  }
  case RTC_ALM_READ: {
    struct rtc_wkalrm_k w;
    int rc = rtc_alarm_read(&w);
    if (rc < 0)
      return rc;
    if (!arg || syscall_copyout(arg, &w.time, sizeof(w.time)) < 0)
      return -EFAULT;
    return 0;
  }
  case RTC_ALM_SET: {
    struct rtc_time_k t;
    if (!arg || syscall_copyin(&t, arg, sizeof(t)) < 0)
      return -EFAULT;
    return rtc_alarm_write(&t, 1);
  }
  case RTC_WKALM_RD: {
    struct rtc_wkalrm_k w;
    int rc = rtc_alarm_read(&w);
    if (rc < 0)
      return rc;
    if (!arg || syscall_copyout(arg, &w, sizeof(w)) < 0)
      return -EFAULT;
    return 0;
  }
  case RTC_WKALM_SET: {
    struct rtc_wkalrm_k w;
    if (!arg || syscall_copyin(&w, arg, sizeof(w)) < 0)
      return -EFAULT;
    return rtc_alarm_write(&w.time, w.enabled);
  }
  case RTC_AIE_ON:
    return rtc_set_status_b(RTC_B_AIE, 1);
  case RTC_AIE_OFF:
    return rtc_set_status_b(RTC_B_AIE, 0);
  case RTC_UIE_ON:
    return rtc_set_status_b(RTC_B_UIE, 1);
  case RTC_UIE_OFF:
    return rtc_set_status_b(RTC_B_UIE, 0);
  case RTC_PIE_ON:
    return rtc_set_status_b(RTC_B_PIE, 1);
  case RTC_PIE_OFF:
    return rtc_set_status_b(RTC_B_PIE, 0);
  case RTC_IRQP_READ: {
    unsigned long f = g_irq_freq;
    if (!arg || syscall_copyout(arg, &f, sizeof(f)) < 0)
      return -EFAULT;
    return 0;
  }
  case RTC_IRQP_SET: {
    unsigned long f = (unsigned long)(usize)arg;
    /* The MC146818 divider only produces powers of two from 2 to 8192. */
    if (f < 2 || f > 8192 || (f & (f - 1)))
      return -EINVAL;
    u32 rate = 1;
    while ((1u << (16 - rate)) != f && rate < 16)
      rate++;
    if (rate >= 16)
      return -EINVAL;
#if defined(__aarch64__)
    /* PL031 has no periodic interrupt source at all — only the alarm match. */
    (void)rate;
    return -EINVAL;
#else
    u64 flags;
    spin_lock_irqsave(&rtc_lock, &flags);
    u8 a = cmos_read(CMOS_STATUS_A);
    cmos_write(CMOS_STATUS_A, (u8)((a & 0xF0) | (rate & 0x0F)));
    spin_unlock_irqrestore(&rtc_lock, flags);
    g_irq_freq = (u32)f;
    return 0;
#endif
  }
  default:
    return -ENOTTY;
  }
}

void rtc_dev_register_nodes(void) {
  static const char *paths[] = {"/dev/rtc0", "/dev/rtc"};
  for (usize i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    struct vfs_node *n = vfs_add_node(paths[i], VFS_DEVICE, 0, 0, 0);
    if (!n || IS_ERR(n))
      continue;
    n->inode->mode = 0644;
    n->inode->read_cb = rtc_node_read;
    n->inode->ioctl_cb = rtc_ioctl;
  }
}

void rtc_dev_init(void) { rtc_dev_register_nodes(); }
