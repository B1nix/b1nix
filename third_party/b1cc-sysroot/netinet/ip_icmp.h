#ifndef B1NIX_U_NETINET_IP_ICMP_H
#define B1NIX_U_NETINET_IP_ICMP_H

#include <stdint.h>
#include <netinet/ip.h> /* struct iphdr, as glibc's ip_icmp.h pulls it in */

/* BSD-style ICMP definitions, enough for BusyBox ping. The b1nix kernel raw
 * ICMP socket delivers replies wrapped in a 20-byte IPv4 header (ihl=5), which
 * ping skips with `iphdr->ihl << 2`. */

/* ICMP types */
#define ICMP_ECHOREPLY       0
#define ICMP_DEST_UNREACH    3
#define ICMP_SOURCE_QUENCH   4
#define ICMP_REDIRECT        5
#define ICMP_ECHO            8
#define ICMP_TIME_EXCEEDED  11
#define ICMP_PARAMETERPROB  12
#define ICMP_TIMESTAMP      13
#define ICMP_TIMESTAMPREPLY 14
#define ICMP_INFO_REQUEST   15
#define ICMP_INFO_REPLY     16
#define ICMP_ADDRESS        17
#define ICMP_ADDRESSREPLY   18

#define ICMP_MINLEN 8

/* Linux struct icmphdr */
struct icmphdr {
  uint8_t type;
  uint8_t code;
  uint16_t checksum;
  union {
    struct {
      uint16_t id;
      uint16_t sequence;
    } echo;
    uint32_t gateway;
    struct {
      uint16_t __unused;
      uint16_t mtu;
    } frag;
  } un;
};

/* BSD struct icmp with the classic icmp_* accessor macros BusyBox ping uses. */
struct icmp {
  uint8_t icmp_type;
  uint8_t icmp_code;
  uint16_t icmp_cksum;
  union {
    struct {
      uint16_t id;
      uint16_t sequence;
    } ih_idseq;
    uint32_t ih_void;
  } icmp_hun;
#define icmp_id   icmp_hun.ih_idseq.id
#define icmp_seq  icmp_hun.ih_idseq.sequence
#define icmp_void icmp_hun.ih_void
  union {
    uint8_t data[1];
  } icmp_dun;
#define icmp_data icmp_dun.data
};

#endif
