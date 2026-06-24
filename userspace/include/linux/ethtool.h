#ifndef _LINUX_ETHTOOL_H
#define _LINUX_ETHTOOL_H

#include <stdint.h>

/* <linux/ethtool.h>: minimal ethtool command interface. b1nix's kernel does
 * not implement SIOCETHTOOL, so the ioctl fails and callers (Chromium net
 * connection-type detection) fall back to a default — the struct/constant only
 * need to exist for the source to compile. Added for the Chromium port
 * (M60-62). Layout matches the Linux UAPI struct ethtool_cmd. */

#define ETHTOOL_GSET 0x00000001  /* DEPRECATED, get settings */

struct ethtool_cmd {
  uint32_t cmd;
  uint32_t supported;
  uint32_t advertising;
  uint16_t speed;
  uint8_t  duplex;
  uint8_t  port;
  uint8_t  phy_address;
  uint8_t  transceiver;
  uint8_t  autoneg;
  uint8_t  mdio_support;
  uint32_t maxtxpkt;
  uint32_t maxrxpkt;
  uint16_t speed_hi;
  uint8_t  eth_tp_mdix;
  uint8_t  eth_tp_mdix_ctrl;
  uint32_t lp_advertising;
  uint32_t reserved[2];
};

/* SPEED_* sentinels referenced by some consumers. */
#define SPEED_10      10
#define SPEED_100     100
#define SPEED_1000    1000
#define SPEED_2500    2500
#define SPEED_10000   10000
#define SPEED_UNKNOWN ((uint16_t)(-1))

#endif /* _LINUX_ETHTOOL_H */
