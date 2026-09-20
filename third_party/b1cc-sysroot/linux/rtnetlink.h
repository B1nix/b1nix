#ifndef B1NIX_U_LINUX_RTNETLINK_H
#define B1NIX_U_LINUX_RTNETLINK_H

#include <linux/types.h>
#include <linux/netlink.h>

/* ── RTM message types ── */
enum {
  RTM_BASE = 16,
#define RTM_BASE RTM_BASE
  RTM_NEWLINK = 16,
  RTM_DELLINK,
  RTM_GETLINK,
  RTM_SETLINK,
  RTM_NEWADDR = 20,
  RTM_DELADDR,
  RTM_GETADDR,
  RTM_NEWROUTE = 24,
  RTM_DELROUTE,
  RTM_GETROUTE,
  RTM_NEWNEIGH = 28,
  RTM_DELNEIGH,
  RTM_GETNEIGH,
  RTM_NEWRULE = 32,
  RTM_DELRULE,
  RTM_GETRULE,
  __RTM_MAX
};
#define RTM_MAX (((__RTM_MAX + 3) & ~3) - 1)

/* ── Generic attribute TLV ── */
struct rtattr {
  unsigned short rta_len;
  unsigned short rta_type;
};

#define RTA_ALIGNTO 4
#define RTA_ALIGN(len) (((len) + RTA_ALIGNTO - 1) & ~(RTA_ALIGNTO - 1))
#define RTA_OK(rta, len)                                                       \
  ((len) >= (int)sizeof(struct rtattr) &&                                      \
   (rta)->rta_len >= sizeof(struct rtattr) && (rta)->rta_len <= (len))
#define RTA_NEXT(rta, attrlen)                                                 \
  ((attrlen) -= RTA_ALIGN((rta)->rta_len),                                     \
   (struct rtattr *)(((char *)(rta)) + RTA_ALIGN((rta)->rta_len)))
#define RTA_LENGTH(len) (RTA_ALIGN(sizeof(struct rtattr)) + (len))
#define RTA_SPACE(len) RTA_ALIGN(RTA_LENGTH(len))
#define RTA_DATA(rta) ((void *)(((char *)(rta)) + RTA_LENGTH(0)))
#define RTA_PAYLOAD(rta) ((int)((rta)->rta_len) - RTA_LENGTH(0))

/* ── Routes ── */
struct rtmsg {
  unsigned char rtm_family;
  unsigned char rtm_dst_len;
  unsigned char rtm_src_len;
  unsigned char rtm_tos;
  unsigned char rtm_table;
  unsigned char rtm_protocol;
  unsigned char rtm_scope;
  unsigned char rtm_type;
  unsigned rtm_flags;
};

enum rtattr_type_t {
  RTA_UNSPEC,
  RTA_DST,
  RTA_SRC,
  RTA_IIF,
  RTA_OIF,
  RTA_GATEWAY,
  RTA_PRIORITY,
  RTA_PREFSRC,
  RTA_METRICS,
  RTA_MULTIPATH,
  RTA_PROTOINFO,
  RTA_FLOW,
  RTA_CACHEINFO,
  RTA_SESSION,
  RTA_MP_ALGO,
  RTA_TABLE,
  RTA_MARK,
  RTA_MFC_STATS,
  RTA_VIA,
  RTA_NEWDST,
  RTA_PREF,
  RTA_ENCAP_TYPE,
  RTA_ENCAP,
  RTA_EXPIRES,
  RTA_PAD,
  RTA_UID,
  RTA_TTL_PROPAGATE,
  RTA_IP_PROTO,
  RTA_SPORT,
  RTA_DPORT,
  RTA_NH_ID,
  __RTA_MAX
};
#define RTA_MAX (__RTA_MAX - 1)

/* Route metrics (nested under RTA_METRICS) */
enum {
  RTAX_UNSPEC,
  RTAX_LOCK,
  RTAX_MTU,
  RTAX_WINDOW,
  RTAX_RTT,
  RTAX_RTTVAR,
  RTAX_SSTHRESH,
  RTAX_CWND,
  RTAX_ADVMSS,
  RTAX_REORDERING,
  RTAX_HOPLIMIT,
  RTAX_INITCWND,
  RTAX_FEATURES,
  RTAX_RTO_MIN,
  RTAX_INITRWND,
  RTAX_QUICKACK,
  RTAX_CC_ALGO,
  RTAX_FASTOPEN_NO_COOKIE,
  __RTAX_MAX
};
#define RTAX_MAX (__RTAX_MAX - 1)
#define RTAX_FEATURE_ECN  (1 << 0)
#define RTAX_FEATURE_SACK (1 << 1)
#define RTAX_FEATURE_TIMESTAMP (1 << 2)
#define RTAX_FEATURE_ALLFRAG   (1 << 3)

#define RTM_RTA(r)                                                             \
  ((struct rtattr *)(((char *)(r)) + NLMSG_ALIGN(sizeof(struct rtmsg))))
#define RTM_PAYLOAD(n) NLMSG_PAYLOAD(n, sizeof(struct rtmsg))

struct rta_cacheinfo {
  __u32 rta_clntref;
  __u32 rta_lastuse;
  __s32 rta_expires;
  __u32 rta_error;
  __u32 rta_used;
  __u32 rta_id;
  __u32 rta_ts;
  __u32 rta_tsage;
};

/* rtm_type */
enum {
  RTN_UNSPEC,
  RTN_UNICAST,
  RTN_LOCAL,
  RTN_BROADCAST,
  RTN_ANYCAST,
  RTN_MULTICAST,
  RTN_BLACKHOLE,
  RTN_UNREACHABLE,
  RTN_PROHIBIT,
  RTN_THROW,
  RTN_NAT,
  RTN_XRESOLVE,
  __RTN_MAX
};
#define RTN_MAX (__RTN_MAX - 1)

/* rtm_protocol */
#define RTPROT_UNSPEC   0
#define RTPROT_REDIRECT 1
#define RTPROT_KERNEL   2
#define RTPROT_BOOT     3
#define RTPROT_STATIC   4
#define RTPROT_DHCP     16

/* rtm_scope */
enum rt_scope_t {
  RT_SCOPE_UNIVERSE = 0,
  RT_SCOPE_SITE = 200,
  RT_SCOPE_LINK = 253,
  RT_SCOPE_HOST = 254,
  RT_SCOPE_NOWHERE = 255
};

/* rtm_table */
enum rt_class_t {
  RT_TABLE_UNSPEC = 0,
  RT_TABLE_DEFAULT = 253,
  RT_TABLE_MAIN = 254,
  RT_TABLE_LOCAL = 255
};

/* rtm_flags */
#define RTM_F_NOTIFY   0x100
#define RTM_F_CLONED   0x200
#define RTM_F_EQUALIZE 0x400

/* nexthop flags */
#define RTNH_F_DEAD       1
#define RTNH_F_PERVASIVE  2
#define RTNH_F_ONLINK     4
#define RTNH_F_OFFLOAD    8
#define RTNH_F_TRAP       64

struct rtnexthop {
  unsigned short rtnh_len;
  unsigned char rtnh_flags;
  unsigned char rtnh_hops;
  int rtnh_ifindex;
};

/* ── Links ── */
struct ifinfomsg {
  unsigned char ifi_family;
  unsigned char __ifi_pad;
  unsigned short ifi_type;
  int ifi_index;
  unsigned ifi_flags;
  unsigned ifi_change;
};

enum {
  IFLA_UNSPEC,
  IFLA_ADDRESS,
  IFLA_BROADCAST,
  IFLA_IFNAME,
  IFLA_MTU,
  IFLA_LINK,
  IFLA_QDISC,
  IFLA_STATS,
  IFLA_COST,
  IFLA_PRIORITY,
  IFLA_MASTER,
  IFLA_WIRELESS,
  IFLA_PROTINFO,
  IFLA_TXQLEN,
  IFLA_MAP,
  IFLA_WEIGHT,
  IFLA_OPERSTATE,
  IFLA_LINKMODE,
  IFLA_LINKINFO,
  IFLA_NET_NS_PID,
  IFLA_IFALIAS,
  IFLA_NUM_VF,
  IFLA_VFINFO_LIST,
  IFLA_STATS64,
  IFLA_VF_PORTS,
  IFLA_PORT_SELF,
  IFLA_AF_SPEC,
  IFLA_GROUP,
  IFLA_NET_NS_FD,
  IFLA_EXT_MASK,
  IFLA_PROMISCUITY,
  IFLA_NUM_TX_QUEUES,
  IFLA_NUM_RX_QUEUES,
  IFLA_CARRIER,
  IFLA_PHYS_PORT_ID,
  __IFLA_MAX
};
#define IFLA_MAX (__IFLA_MAX - 1)

#define IFLA_RTA(r)                                                            \
  ((struct rtattr *)(((char *)(r)) + NLMSG_ALIGN(sizeof(struct ifinfomsg))))
#define IFLA_PAYLOAD(n) NLMSG_PAYLOAD(n, sizeof(struct ifinfomsg))

enum {
  IFLA_INFO_UNSPEC,
  IFLA_INFO_KIND,
  IFLA_INFO_DATA,
  IFLA_INFO_XSTATS,
  __IFLA_INFO_MAX
};
#define IFLA_INFO_MAX (__IFLA_INFO_MAX - 1)

/* ── Addresses ── */
struct ifaddrmsg {
  __u8 ifa_family;
  __u8 ifa_prefixlen;
  __u8 ifa_flags;
  __u8 ifa_scope;
  __u32 ifa_index;
};

enum {
  IFA_UNSPEC,
  IFA_ADDRESS,
  IFA_LOCAL,
  IFA_LABEL,
  IFA_BROADCAST,
  IFA_ANYCAST,
  IFA_CACHEINFO,
  IFA_MULTICAST,
  IFA_FLAGS,
  __IFA_MAX
};
#define IFA_MAX (__IFA_MAX - 1)

/* ifa_flags */
#define IFA_F_SECONDARY   0x01
#define IFA_F_TEMPORARY   IFA_F_SECONDARY
#define IFA_F_NODAD       0x02
#define IFA_F_OPTIMISTIC  0x04
#define IFA_F_DADFAILED   0x08
#define IFA_F_HOMEADDRESS 0x10
#define IFA_F_DEPRECATED  0x20
#define IFA_F_TENTATIVE   0x40
#define IFA_F_PERMANENT   0x80

#define IFA_RTA(r)                                                             \
  ((struct rtattr *)(((char *)(r)) + NLMSG_ALIGN(sizeof(struct ifaddrmsg))))
#define IFA_PAYLOAD(n) NLMSG_PAYLOAD(n, sizeof(struct ifaddrmsg))

struct ifa_cacheinfo {
  __u32 ifa_prefered;
  __u32 ifa_valid;
  __u32 cstamp;
  __u32 tstamp;
};

/* ── Dump-request bodies ── */
struct rtgenmsg {
  unsigned char rtgen_family;
};

/* Multicast groups (legacy) */
#define RTMGRP_LINK        1
#define RTMGRP_NOTIFY      2
#define RTMGRP_NEIGH       4
#define RTMGRP_TC          8
#define RTMGRP_IPV4_IFADDR 0x10
#define RTMGRP_IPV4_MROUTE 0x20
#define RTMGRP_IPV4_ROUTE  0x40
#define RTMGRP_IPV4_RULE   0x80
#define RTMGRP_IPV6_IFADDR 0x100
#define RTMGRP_IPV6_MROUTE 0x200
#define RTMGRP_IPV6_ROUTE  0x400
#define RTMGRP_IPV6_IFINFO 0x800
#define RTMGRP_IPV6_PREFIX 0x20000

#endif
