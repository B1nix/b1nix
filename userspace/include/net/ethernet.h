#ifndef B1NIX_U_NET_ETHERNET_H
#define B1NIX_U_NET_ETHERNET_H

#include <stdint.h>
#include <sys/types.h>

/* Minimal <net/ethernet.h> for BusyBox interface/ifconfig (struct ether_addr,
 * ETH_ALEN). */
#define ETH_ALEN       6
#define ETHER_ADDR_LEN 6

struct ether_addr {
  uint8_t ether_addr_octet[ETH_ALEN];
} __attribute__((packed));

struct ether_header {
  uint8_t ether_dhost[ETH_ALEN];
  uint8_t ether_shost[ETH_ALEN];
  uint16_t ether_type;
} __attribute__((packed));

#define ETHERTYPE_IP   0x0800
#define ETHERTYPE_ARP  0x0806
#define ETHERTYPE_IPV6 0x86DD

#endif
