#ifndef B1NIX_U_LINUX_NETLINK_H
#define B1NIX_U_LINUX_NETLINK_H

#include <linux/types.h>

#define NETLINK_ROUTE 0

struct sockaddr_nl {
  __kernel_sa_family_t nl_family;
  unsigned short nl_pad;
  __u32 nl_pid;
  __u32 nl_groups;
};

struct nlmsghdr {
  __u32 nlmsg_len;
  __u16 nlmsg_type;
  __u16 nlmsg_flags;
  __u32 nlmsg_seq;
  __u32 nlmsg_pid;
};

/* nlmsg_flags */
#define NLM_F_REQUEST 0x01
#define NLM_F_MULTI   0x02
#define NLM_F_ACK     0x04
#define NLM_F_ECHO    0x08
#define NLM_F_DUMP_INTR 0x10
/* GET request modifiers */
#define NLM_F_ROOT    0x100
#define NLM_F_MATCH   0x200
#define NLM_F_ATOMIC  0x400
#define NLM_F_DUMP    (NLM_F_ROOT | NLM_F_MATCH)
/* NEW request modifiers */
#define NLM_F_REPLACE 0x100
#define NLM_F_EXCL    0x200
#define NLM_F_CREATE  0x400
#define NLM_F_APPEND  0x800

#define NLMSG_NOOP    0x1
#define NLMSG_ERROR   0x2
#define NLMSG_DONE    0x3
#define NLMSG_OVERRUN 0x4
#define NLMSG_MIN_TYPE 0x10

#define NLMSG_ALIGNTO 4U
#define NLMSG_ALIGN(len) (((len) + NLMSG_ALIGNTO - 1) & ~(NLMSG_ALIGNTO - 1))
#define NLMSG_HDRLEN ((int)NLMSG_ALIGN(sizeof(struct nlmsghdr)))
#define NLMSG_LENGTH(len) ((len) + NLMSG_HDRLEN)
#define NLMSG_SPACE(len) NLMSG_ALIGN(NLMSG_LENGTH(len))
#define NLMSG_DATA(nlh) ((void *)(((char *)nlh) + NLMSG_HDRLEN))
#define NLMSG_NEXT(nlh, len)                                                   \
  ((len) -= NLMSG_ALIGN((nlh)->nlmsg_len),                                     \
   (struct nlmsghdr *)(((char *)(nlh)) + NLMSG_ALIGN((nlh)->nlmsg_len)))
#define NLMSG_OK(nlh, len)                                                     \
  ((len) >= (int)sizeof(struct nlmsghdr) &&                                    \
   (nlh)->nlmsg_len >= sizeof(struct nlmsghdr) && (nlh)->nlmsg_len <= (len))
#define NLMSG_PAYLOAD(nlh, len) ((nlh)->nlmsg_len - NLMSG_SPACE((len)))

struct nlmsgerr {
  int error;
  struct nlmsghdr msg;
};

struct nlattr {
  __u16 nla_len;
  __u16 nla_type;
};

#define NLA_ALIGNTO    4
#define NLA_ALIGN(len) (((len) + NLA_ALIGNTO - 1) & ~(NLA_ALIGNTO - 1))
#define NLA_HDRLEN     ((int)NLA_ALIGN(sizeof(struct nlattr)))

#endif
