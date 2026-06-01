#include <b1nix/net.h>
#include <b1nix/rtc.h>
#include <b1nix/sched.h>
#include <string.h>

#define NTP_PORT 123
#define NTP_SRC_PORT 12346
#define NTP_UNIX_EPOCH_DELTA 2208988800UL
#define NTP_MAX_STRATUM 15
#define NTP_MAX_ABS_OFFSET_SEC (24u * 60u * 60u)
#define NTP_STEP_THRESHOLD_SEC 3u
#define NTP_SLEW_APPLY_TICKS 100u /* ~1s at 100Hz */

static volatile int ntp_inflight = 0;
static volatile int ntp_synced = 0;
static u64 ntp_last_send_ticks = 0;
static u64 ntp_last_try_ticks = 0;
static u64 ntp_last_slew_ticks = 0;
static int ntp_registered = 0;
static int ntp_pending_slew_sec = 0;

static u16 bswap16(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static u32 bswap32(u32 v) {
  return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
         ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

static void ntp_receive(const void *data, usize size) {
  if (size < 48) return;
  const u8 *p = (const u8 *)data;
  u8 mode = p[0] & 0x07;
  if (mode != 4 && mode != 5) return; /* server or broadcast */
  u8 stratum = p[1];
  if (stratum == 0 || stratum > NTP_MAX_STRATUM) return;

  u32 tx_secs = ((u32)p[40] << 24) | ((u32)p[41] << 16) | ((u32)p[42] << 8) | p[43];
  if (tx_secs < NTP_UNIX_EPOCH_DELTA) return;
  u32 remote_unix = tx_secs - NTP_UNIX_EPOCH_DELTA;
  u32 local_unix = rtc_now_unix_seconds();
  long long delta = (long long)remote_unix - (long long)local_unix;
  unsigned long long abs_delta =
      (delta < 0) ? (unsigned long long)(-delta) : (unsigned long long)delta;
  if (abs_delta > NTP_MAX_ABS_OFFSET_SEC) return;

  if (abs_delta >= NTP_STEP_THRESHOLD_SEC) {
    rtc_set_unix_time(remote_unix);
    ntp_pending_slew_sec = 0;
  } else if (delta != 0) {
    if (delta > 0) ntp_pending_slew_sec += (int)delta;
    else ntp_pending_slew_sec -= (int)(-delta);
  }

  ntp_synced = 1;
  ntp_inflight = 0;
}

static int ntp_send_query(void) {
  u8 ip[4];
  if (dns_resolve_sync("pool.ntp.org", ip) != 0) return -1;
  struct ipv4_addr server = {{ip[0], ip[1], ip[2], ip[3]}};

  u8 pkt[48];
  memset(pkt, 0, sizeof(pkt));
  pkt[0] = 0x1b; /* LI=0, VN=3, Mode=3(client) */
  u32 tx = rtc_now_unix_seconds() + NTP_UNIX_EPOCH_DELTA;
  tx = bswap32(tx);
  memcpy(pkt + 40, &tx, sizeof(tx));

  udp_send_net(server, bswap16(NTP_SRC_PORT), bswap16(NTP_PORT), pkt, sizeof(pkt));
  ntp_inflight = 1;
  ntp_last_send_ticks = scheduler_get_uptime_ticks();
  return 0;
}

void ntp_tick(u64 now_ticks) {
  if (!ntp_registered) {
    udp_register_handler(NTP_SRC_PORT, ntp_receive);
    ntp_registered = 1;
  }

  if (ntp_pending_slew_sec != 0 && now_ticks - ntp_last_slew_ticks >= NTP_SLEW_APPLY_TICKS) {
    u32 now = rtc_now_unix_seconds();
    if (ntp_pending_slew_sec > 0) {
      rtc_set_unix_time(now + 1);
      ntp_pending_slew_sec--;
    } else {
      rtc_set_unix_time(now - 1);
      ntp_pending_slew_sec++;
    }
    ntp_last_slew_ticks = now_ticks;
  }

  if (ntp_inflight) {
    if (now_ticks - ntp_last_send_ticks > 500) {
      ntp_inflight = 0; /* timeout, retry later */
    }
    return;
  }
  /* First successful sync as soon as networking is up, then periodic refresh. */
  u64 interval = ntp_synced ? 360000 : 2000; /* ~1h or ~20s at 100Hz */
  if (now_ticks - ntp_last_try_ticks < interval) return;
  ntp_last_try_ticks = now_ticks;
  (void)ntp_send_query();
}
