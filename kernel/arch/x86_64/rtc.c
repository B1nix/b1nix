#include <b1nix/io.h>
#include <b1nix/rtc.h>
#include <b1nix/ktime.h>
#include <b1nix/sched.h>
#include <b1nix/console.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

u64 rtc_boot_time_seconds = 0;
/* Applied to the wall clock by settimeofday(2), in nanoseconds. */
static i64 rtc_time_offset_ns = 0;

static u8 read_cmos(u8 reg) {
  outb(CMOS_ADDR, reg);
  return inb(CMOS_DATA);
}

static int is_updating() {
  outb(CMOS_ADDR, 0x0A);
  return (inb(CMOS_DATA) & 0x80);
}


void rtc_init(void) {
  /* Bounded UIP wait: a dead/absent RTC or a wedged southbridge can leave the
   * update-in-progress bit stuck forever; an unbounded spin here would hang the
   * boot before the test 'done' marker (the 120s policy). Proceed regardless
   * after a generous spin — a slightly-torn read is far better than a hang. */
  for (int i = 0; i < 1000000 && is_updating(); i++)
    ;

  u8 second = read_cmos(0x00);
  u8 minute = read_cmos(0x02);
  u8 hour = read_cmos(0x04);
  u8 day = read_cmos(0x07);
  u8 month = read_cmos(0x08);
  u16 year = read_cmos(0x09);
  /* CMOS 0x32 is the century register on every PC-compatible southbridge
   * (QEMU included) and is what the FADT's century index points at when
   * firmware declares one. It is read here rather than through ACPI because
   * rtc_init runs from arch_init, before acpi_init has parsed any table. A
   * value outside 19..21 means the register is absent or holding something
   * else, and the two-digit year is then windowed the usual way below. */
  u8 century = read_cmos(0x32);

  u8 registerB = read_cmos(0x0B);

  // Convert BCD to binary if necessary
  if (!(registerB & 0x04)) {
    second = (second & 0x0F) + ((second / 16) * 10);
    minute = (minute & 0x0F) + ((minute / 16) * 10);
    hour = ((hour & 0x0F) + (((hour & 0x70) / 16) * 10)) | (hour & 0x80);
    day = (day & 0x0F) + ((day / 16) * 10);
    month = (month & 0x0F) + ((month / 16) * 10);
    year = (year & 0x0F) + ((year / 16) * 10);
    century = (u8)((century & 0x0F) + ((century / 16) * 10));
  }

  // Convert 12 hour clock to 24 hour clock if necessary
  if (!(registerB & 0x02) && (hour & 0x80)) {
    hour = ((hour & 0x7F) + 12) % 24;
  }
  hour &= 0x7F; /* the PM flag is not part of the hour in either mode */

  /* A dead or torn RTC read must not drive the civil-date arithmetic with
   * nonsense: fall back to the epoch date. The century is dropped along with
   * the rest, so the windowing below reads 70 as 1970 rather than 2070. */
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
      minute > 59 || second > 60) {
    month = 1;
    day = 1;
    hour = minute = second = 0;
    year = 70;
    century = 0;
  }

  /* Full year. The CMOS year register holds two digits; the century comes from
   * 0x32 when that register is populated, and from the standard 1970 window
   * (69 → 2069, 70 → 1970) when it is not. */
  if (century >= 19 && century <= 21)
    year = (u16)(century * 100 + year);
  else
    year = (u16)(year < 70 ? year + 2000 : year + 1900);

  rtc_boot_time_seconds = rtc_civil_to_unix(year, month, day, hour, minute, second);
}


/* The wall clock, in nanoseconds since the epoch.
 *
 * ONE value, from which both the seconds and the sub-second part are taken.
 * They used to come from different clocks — the seconds counted 100 Hz ticks
 * while clock_gettime(CLOCK_REALTIME) took its nanoseconds from the TSC — and
 * the two are not in phase, so the composite walked BACKWARDS by up to a
 * second, over and over, all boot long. systemd-journald noticed ("Time jumped
 * backwards, rotating") and threw its journal away each time; every file
 * timestamp and every timeout computed from the wall clock was equally
 * unreliable. */
u64 rtc_now_unix_nanos(void) {
  i64 ns = (i64)(rtc_boot_time_seconds * 1000000000ull) +
           (i64)ktime_monotonic_ns() + rtc_time_offset_ns;
  return ns < 0 ? 0 : (u64)ns;
}

u64 rtc_now_unix_seconds(void) { return rtc_now_unix_nanos() / 1000000000ull; }

void rtc_set_unix_time(u64 unix_time_now) {
  i64 base = (i64)(rtc_boot_time_seconds * 1000000000ull) +
             (i64)ktime_monotonic_ns();
  rtc_time_offset_ns = (i64)(unix_time_now * 1000000000ull) - base;
}
