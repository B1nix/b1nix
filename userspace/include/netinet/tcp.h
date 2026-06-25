#ifndef B1NIX_U_NETINET_TCP_H
#define B1NIX_U_NETINET_TCP_H

#include <sys/socket.h>  /* IPPROTO_TCP */

#define TCP_NODELAY  1
#define TCP_MAXSEG   2
#define TCP_CORK     3
#define TCP_INFO     11   /* getsockopt tcp_info (named by seccomp policy; b1nix has no tcp_info backend) */
#define TCP_KEEPIDLE  4   /* idle seconds before keepalive probes */
#define TCP_KEEPINTVL 5   /* seconds between keepalive probes */
#define TCP_KEEPCNT   6   /* probes before declaring the peer dead */
#define TCP_USER_TIMEOUT 18  /* max time before unacked data forces close (ms) */

#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif

/* struct tcp_info — getsockopt(TCP_INFO) statistics (Linux ABI). b1nix has no
 * TCP_INFO backend yet (the getsockopt returns an error, which callers handle),
 * but real net code guards on `#if defined(TCP_INFO)` and needs the struct to
 * compile once TCP_INFO is defined. Full modern layout. */
#include <stdint.h>
struct tcp_info {
    uint8_t  tcpi_state;
    uint8_t  tcpi_ca_state;
    uint8_t  tcpi_retransmits;
    uint8_t  tcpi_probes;
    uint8_t  tcpi_backoff;
    uint8_t  tcpi_options;
    uint8_t  tcpi_snd_wscale : 4, tcpi_rcv_wscale : 4;
    uint8_t  tcpi_delivery_rate_app_limited : 1, tcpi_fastopen_client_fail : 2;
    uint32_t tcpi_rto;
    uint32_t tcpi_ato;
    uint32_t tcpi_snd_mss;
    uint32_t tcpi_rcv_mss;
    uint32_t tcpi_unacked;
    uint32_t tcpi_sacked;
    uint32_t tcpi_lost;
    uint32_t tcpi_retrans;
    uint32_t tcpi_fackets;
    uint32_t tcpi_last_data_sent;
    uint32_t tcpi_last_ack_sent;
    uint32_t tcpi_last_data_recv;
    uint32_t tcpi_last_ack_recv;
    uint32_t tcpi_pmtu;
    uint32_t tcpi_rcv_ssthresh;
    uint32_t tcpi_rtt;
    uint32_t tcpi_rttvar;
    uint32_t tcpi_snd_ssthresh;
    uint32_t tcpi_snd_cwnd;
    uint32_t tcpi_advmss;
    uint32_t tcpi_reordering;
    uint32_t tcpi_rcv_rtt;
    uint32_t tcpi_rcv_space;
    uint32_t tcpi_total_retrans;
    uint64_t tcpi_pacing_rate;
    uint64_t tcpi_max_pacing_rate;
    uint64_t tcpi_bytes_acked;
    uint64_t tcpi_bytes_received;
    uint32_t tcpi_segs_out;
    uint32_t tcpi_segs_in;
    uint32_t tcpi_notsent_bytes;
    uint32_t tcpi_min_rtt;
    uint32_t tcpi_data_segs_in;
    uint32_t tcpi_data_segs_out;
    uint64_t tcpi_delivery_rate;
    uint64_t tcpi_busy_time;
    uint64_t tcpi_rwnd_limited;
    uint64_t tcpi_sndbuf_limited;
    uint32_t tcpi_delivered;
    uint32_t tcpi_delivered_ce;
    uint64_t tcpi_bytes_sent;
    uint64_t tcpi_bytes_retrans;
    uint32_t tcpi_dsack_dups;
    uint32_t tcpi_reord_seen;
    uint32_t tcpi_rcv_ooopack;
    uint32_t tcpi_snd_wnd;
};

#endif
