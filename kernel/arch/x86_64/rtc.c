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

/* Seconds since 1970-01-01 00:00:00 UTC for a proleptic-Gregorian civil date.
 *
 * Exact, century rule included. The version this replaced counted leap days as
 * (y + 2) / 4 and tested February with `year % 4 == 0`, which is right only
 * between 1901 and 2099 and even there only outside January and February of a
 * leap year — it reported those two months a day late, every leap year. Every
 * file timestamp, every timeout and every certificate validity check on the
 * machine is derived from this number, so "good enough for a boot offset" was
 * not good enough.
 *
 * The shift-to-March algorithm (Howard Hinnant's days_from_civil) does it in
 * closed form: numbering March as month 1 puts the leap day at the END of the
 * year, which makes the month-length pattern the exact linear (153m + 2) / 5
 * and leaves the leap-day count to plain integer division over a 400-year era.
 * A date before the epoch returns 0 — the callers here (a boot-time clock read
 * and its self-test) have nothing useful to do with a negative wall clock. */
u64 rtc_civil_to_unix(u16 year, u32 month, u32 day, u32 hour, u32 minute,
                      u32 second) {
  i64 y = (i64)year;
  u32 m = month;
  if (m <= 2) {
    y -= 1;
    m += 12;
  }
  i64 era = (y >= 0 ? y : y - 399) / 400;
  u64 yoe = (u64)(y - era * 400);                  /* year of era, 0..399 */
  u64 doy = (153u * (m - 3) + 2) / 5 + day - 1;    /* day of year, Mar 1 = 0 */
  u64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; /* day of era, 0..146096 */
  i64 days = era * 146097 + (i64)doe - 719468;     /* 719468 = 1970-01-01 */

  if (days < 0)
    return 0;

  return (u64)days * 86400ull + (u64)hour * 3600ull + (u64)minute * 60ull +
         second;
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

/* Known-answer test for the civil-date conversion, run in test mode.
 *
 * The vectors are the cases the old approximation got wrong, plus the two
 * boundaries it could never have reached: 2000 is a leap year (divisible by
 * 400) while 2100 is not (divisible by 100 but not 400), and February of a leap
 * year is where the (y + 2) / 4 estimate slipped a day. Values cross-checked
 * against `date -u -d … +%s`. */
void rtc_selftest(void) {
  static const struct {
    u16 year;
    u32 month, day, hour, minute, second;
    u64 expect;
    const char *name;
  } vectors[] = {
      {1970, 1, 1, 0, 0, 0, 0ull, "epoch"},
      {2001, 9, 9, 1, 46, 40, 1000000000ull, "billennium"},
      /* Feb 29 exists only because 2000 is a 400-year leap year. */
      {2000, 2, 29, 0, 0, 0, 951782400ull, "leap-2000"},
      /* January of a leap year — the month the old estimate reported late. */
      {2024, 1, 31, 12, 0, 0, 1706702400ull, "leap-year-january"},
      {2024, 2, 29, 23, 59, 59, 1709251199ull, "leap-2024"},
      /* 2100 is NOT a leap year: March 1 follows February 28. */
      {2100, 3, 1, 0, 0, 0, 4107542400ull, "century-2100"},
  };

  for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
    u64 got = rtc_civil_to_unix(vectors[i].year, vectors[i].month, vectors[i].day,
                                vectors[i].hour, vectors[i].minute,
                                vectors[i].second);
    if (got == vectors[i].expect) {
      console_write("M118-RTC: ok ");
      console_write(vectors[i].name);
      console_write("\n");
    } else {
      console_write("M118-RTC: FAIL ");
      console_write(vectors[i].name);
      console_write(" got=");
      console_write_dec(got);
      console_write(" want=");
      console_write_dec(vectors[i].expect);
      console_write("\n");
    }
  }
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
