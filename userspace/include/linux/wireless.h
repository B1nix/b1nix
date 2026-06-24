#ifndef _LINUX_WIRELESS_H
#define _LINUX_WIRELESS_H

#include <stdint.h>
#include <net/if.h>   /* IFNAMSIZ */

/* <linux/wireless.h>: a minimal subset of the Wireless Extensions interface.
 * b1nix has no wireless stack, so the SIOCGIW* ioctls fail and callers
 * (Chromium net connection-type/SSID detection) fall back to wired/unknown.
 * The struct/constant only need to exist for the source to compile; the layout
 * matches the Linux UAPI (struct iwreq / iw_point). Added for the Chromium
 * port (M60-62). */

#define IW_ESSID_MAX_SIZE 32

struct iw_point {
  void    *pointer;
  uint16_t length;
  uint16_t flags;
};

struct iw_param {
  int32_t  value;
  uint8_t  fixed;
  uint8_t  disabled;
  uint16_t flags;
};

struct iw_freq {
  int32_t  m;
  int16_t  e;
  uint8_t  i;
  uint8_t  flags;
};

struct iw_quality {
  uint8_t qual;
  uint8_t level;
  uint8_t noise;
  uint8_t updated;
};

union iwreq_data {
  char              name[IFNAMSIZ];
  struct iw_point   essid;
  struct iw_param   nwid;
  struct iw_freq    freq;
  struct iw_param   sens;
  struct iw_param   bitrate;
  struct iw_param   txpower;
  struct iw_param   rts;
  struct iw_param   frag;
  uint32_t          mode;
  struct iw_param   retry;
  struct iw_point   encoding;
  struct iw_param   power;
  struct iw_quality qual;
  struct sockaddr   ap_addr;
  struct sockaddr   addr;
  struct iw_param   param;
  struct iw_point   data;
};

struct iwreq {
  union {
    char ifrn_name[IFNAMSIZ];
  } ifr_ifrn;
  union iwreq_data u;
};

#define ifr_name ifr_ifrn.ifrn_name

#endif /* _LINUX_WIRELESS_H */
