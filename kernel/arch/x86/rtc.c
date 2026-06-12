#include <b1nix/io.h>
#include <b1nix/rtc.h>
#include <b1nix/sched.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

u64 rtc_boot_time_seconds = 0;
static i64 rtc_time_offset_seconds = 0;

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

  u8 registerB = read_cmos(0x0B);

  // Convert BCD to binary if necessary
  if (!(registerB & 0x04)) {
    second = (second & 0x0F) + ((second / 16) * 10);
    minute = (minute & 0x0F) + ((minute / 16) * 10);
    hour = ((hour & 0x0F) + (((hour & 0x70) / 16) * 10)) | (hour & 0x80);
    day = (day & 0x0F) + ((day / 16) * 10);
    month = (month & 0x0F) + ((month / 16) * 10);
    year = (year & 0x0F) + ((year / 16) * 10);
  }

  // Convert 12 hour clock to 24 hour clock if necessary
  if (!(registerB & 0x02) && (hour & 0x80)) {
    hour = ((hour & 0x7F) + 12) % 24;
  }

  // Calculate year
  year += 2000;

  // Simple Unix time calculation
  u64 y = year - 1970;
  u64 d = y * 365 + (y + 2) / 4; // approximate leap years since 1970
  
  static const u16 month_days[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  d += month_days[month - 1];
  if (month > 2 && (year % 4 == 0)) d++;
  d += day - 1;

  rtc_boot_time_seconds = d * 86400 + hour * 3600 + minute * 60 + second;
}

u64 rtc_now_unix_seconds(void) {
  u64 base = rtc_boot_time_seconds + (scheduler_get_uptime_ticks() / 100);
  i64 adjusted = (i64)base + rtc_time_offset_seconds;
  if (adjusted < 0) return 0;
  return (u64)adjusted;
}

void rtc_set_unix_time(u64 unix_time_now) {
  u64 base = rtc_boot_time_seconds + (scheduler_get_uptime_ticks() / 100);
  rtc_time_offset_seconds = (i64)unix_time_now - (i64)base;
}
