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
#define NTP_INITIAL_RETRY_TICKS 2000u
#define NTP_MAX_RETRY_TICKS 60000u
#define NTP_PERIODIC_SYNC_TICKS 360000u
#define NTP_INFLIGHT_TIMEOUT_TICKS 500u
#define NTP_ERA_SECONDS (1ULL << 32)

static volatile int ntp_inflight = 0;
static volatile int ntp_synced = 0;
static u64 ntp_last_send_ticks = 0;
static u64 ntp_last_try_ticks = 0;
static u64 ntp_last_slew_ticks = 0;
static int ntp_registered = 0;
static int ntp_pending_slew_sec = 0;
static u32 ntp_last_client_tx_secs = 0;
static u32 ntp_retry_ticks = NTP_INITIAL_RETRY_TICKS;
/* M96 module parameter: the server the client resolves. Writable through
 * /sys/module/ntp/parameters/ntp_server_name; a change clears the cached
 * address so the next query re-resolves. */
static char ntp_server_name[64] = "pool.ntp.org";
static int ntp_server_cached = 0;
static struct ipv4_addr ntp_server_ip = {{0, 0, 0, 0}};

static u16 bswap16(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static u32 bswap32(u32 v) {
  return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
         ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

static void ntp_receive(const void *data, usize size) {
  if (size < 48) return;
  const u8 *p = (const u8 *)data;
  u8 li = (p[0] >> 6) & 0x03;
  u8 mode = p[0] & 0x07;
  if (li == 3) return;            /* alarm condition: clock unsynchronized */
  if (mode != 4 && mode != 5) return; /* server or broadcast */
  u8 stratum = p[1];
  if (stratum == 0 || stratum > NTP_MAX_STRATUM) return;

  u32 org_secs = ((u32)p[24] << 24) | ((u32)p[25] << 16) | ((u32)p[26] << 8) | p[27];
  if (ntp_last_client_tx_secs != 0 && org_secs != ntp_last_client_tx_secs) return;

  u32 tx_secs = ((u32)p[40] << 24) | ((u32)p[41] << 16) | ((u32)p[42] << 8) | p[43];
  u64 local_ntp = rtc_now_unix_seconds() + NTP_UNIX_EPOCH_DELTA;
  u64 base = local_ntp & ~(NTP_ERA_SECONDS - 1ULL);
  u64 c0 = base | (u64)tx_secs;
  u64 c_prev = (base >= NTP_ERA_SECONDS) ? (c0 - NTP_ERA_SECONDS) : c0;
  u64 c_next = c0 + NTP_ERA_SECONDS;
  u64 d0 = (c0 > local_ntp) ? (c0 - local_ntp) : (local_ntp - c0);
  u64 dp = (c_prev > local_ntp) ? (c_prev - local_ntp) : (local_ntp - c_prev);
  u64 dn = (c_next > local_ntp) ? (c_next - local_ntp) : (local_ntp - c_next);
  u64 full_ntp = c0;
  if (dp < d0 && dp <= dn) full_ntp = c_prev;
  else if (dn < d0 && dn < dp) full_ntp = c_next;
  if (full_ntp < NTP_UNIX_EPOCH_DELTA) return;
  u64 remote_unix = full_ntp - NTP_UNIX_EPOCH_DELTA;
  u64 local_unix = rtc_now_unix_seconds();
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
  ntp_retry_ticks = NTP_INITIAL_RETRY_TICKS;
}

static int ntp_send_query(void) {
  if (!ntp_server_cached) {
    u8 ip[4];
    if (dns_resolve_sync_quiet(ntp_server_name, ip) != 0) return -1;
    ntp_server_ip.bytes[0] = ip[0];
    ntp_server_ip.bytes[1] = ip[1];
    ntp_server_ip.bytes[2] = ip[2];
    ntp_server_ip.bytes[3] = ip[3];
    ntp_server_cached = 1;
  }

  u8 pkt[48];
  memset(pkt, 0, sizeof(pkt));
  pkt[0] = 0x1b; /* LI=0, VN=3, Mode=3(client) */
  u32 tx_host = (u32)(rtc_now_unix_seconds() + NTP_UNIX_EPOCH_DELTA);
  ntp_last_client_tx_secs = tx_host;
  u32 tx_net = bswap32(tx_host);
  memcpy(pkt + 40, &tx_net, sizeof(tx_net));

  udp_send_net(ntp_server_ip, bswap16(NTP_SRC_PORT), bswap16(NTP_PORT), pkt, sizeof(pkt));
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
    u64 now = rtc_now_unix_seconds();
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
    if (now_ticks - ntp_last_send_ticks > NTP_INFLIGHT_TIMEOUT_TICKS) {
      ntp_inflight = 0; /* timeout, retry later */
      if (!ntp_synced) {
        if (ntp_retry_ticks < NTP_MAX_RETRY_TICKS) {
          u32 next = ntp_retry_ticks << 1;
          ntp_retry_ticks = (next > NTP_MAX_RETRY_TICKS) ? NTP_MAX_RETRY_TICKS : next;
        }
      }
      ntp_server_cached = 0; /* force re-resolve on next query */
    }
    return;
  }
  /* First successful sync as soon as networking is up, then periodic refresh. */
  u64 interval = ntp_synced ? NTP_PERIODIC_SYNC_TICKS : ntp_retry_ticks;
  if (now_ticks - ntp_last_try_ticks < interval) return;
  ntp_last_try_ticks = now_ticks;
  if (ntp_send_query() != 0) {
    ntp_server_cached = 0;
    if (!ntp_synced && ntp_retry_ticks < NTP_MAX_RETRY_TICKS) {
      u32 next = ntp_retry_ticks << 1;
      ntp_retry_ticks = (next > NTP_MAX_RETRY_TICKS) ? NTP_MAX_RETRY_TICKS : next;
    }
  }
}

/* ── M96: the SNTP client is a loadable module ───────────────────────────── */
#include <b1nix/module.h>
#include <b1nix/netproto.h>

MODULE_NAME("ntp");
MODULE_LICENSE("MIT");
MODULE_AUTHOR("b1nix");
MODULE_DESCRIPTION("SNTP client: steps or slews the RTC from a time server");
MODULE_ALIAS("net-time-sntp");

/* The pool hostname the client resolves. Writable, so a boot on a network with
 * its own time server can be pointed at it without a rebuild. */
module_param_desc(ntp_server_name, MODULE_PARAM_STRING, 0644,
                  "hostname of the SNTP server to query");

static struct net_proto ntp_proto = {
	.name = "ntp",
	.tick = ntp_tick,
};

static int ntp_module_init(void) { return proto_register(&ntp_proto); }

static void ntp_module_exit(void) {
	proto_unregister(&ntp_proto);
	if (ntp_registered) {
		udp_unregister_handler(NTP_SRC_PORT);
		ntp_registered = 0;
	}
}

module_init(ntp_module_init);
module_exit(ntp_module_exit);
