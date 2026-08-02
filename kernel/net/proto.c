/* Network protocol registry (M96).
 *
 * A tiny publish/dispatch layer that lets the IPv6 datapath, Neighbour
 * Discovery and the SNTP client live in loadable modules while the core stack
 * keeps a single, stable set of call sites.
 */

#include <b1nix/errno.h>
#include <b1nix/netproto.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <string.h>

static struct net_proto *proto_list;
static spinlock_t proto_lock = SPINLOCK_INIT;

/* Dispatch guard. A protocol module is unloaded while the net daemon may be
 * anywhere inside one of its hooks; the hooks send packets, so they cannot run
 * under a spinlock. Instead every dispatcher brackets its iteration with this
 * counter, and proto_unregister — after unlinking, so no new dispatch can find
 * the entry — waits for the count to drain before returning. The module's exit
 * path only frees its text once that has happened. */
static volatile int proto_dispatch_depth;

static void proto_dispatch_enter(void) {
  __atomic_fetch_add(&proto_dispatch_depth, 1, __ATOMIC_ACQUIRE);
}

static void proto_dispatch_leave(void) {
  __atomic_fetch_sub(&proto_dispatch_depth, 1, __ATOMIC_RELEASE);
}

int proto_register(struct net_proto *proto) {
  if (!proto || !proto->name)
    return -EINVAL;
  u64 flags;
  spin_lock_irqsave(&proto_lock, &flags);
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p == proto || strcmp(p->name, proto->name) == 0) {
      spin_unlock_irqrestore(&proto_lock, flags);
      return -EEXIST;
    }
  }
  proto->next = proto_list;
  proto_list = proto;
  spin_unlock_irqrestore(&proto_lock, flags);
  return 0;
}

void proto_unregister(struct net_proto *proto) {
  if (!proto)
    return;
  u64 flags;
  spin_lock_irqsave(&proto_lock, &flags);
  struct net_proto **pp = &proto_list;
  while (*pp) {
    if (*pp == proto) {
      *pp = proto->next;
      proto->next = 0;
      break;
    }
    pp = &(*pp)->next;
  }
  spin_unlock_irqrestore(&proto_lock, flags);

  /* Wait out any dispatcher that entered before the unlink. */
  while (__atomic_load_n(&proto_dispatch_depth, __ATOMIC_ACQUIRE) > 0)
    scheduler_yield();
}

struct net_proto *proto_find(const char *name) {
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (strcmp(p->name, name) == 0)
      return p;
  }
  return 0;
}

usize proto_count(void) {
  usize n = 0;
  for (struct net_proto *p = proto_list; p; p = p->next)
    n++;
  return n;
}

const char *proto_name_at(usize index) {
  usize n = 0;
  for (struct net_proto *p = proto_list; p; p = p->next, n++) {
    if (n == index)
      return p->name;
  }
  return 0;
}

int proto_deliver_ether(u16 ether_type, const void *data, usize size) {
  int delivered = 0;
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->ether_type == ether_type && p->receive) {
      p->receive(data, size);
      delivered = 1;
    }
  }
  proto_dispatch_leave();
  return delivered;
}

void net_proto_tick(u64 now_ticks) {
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->tick)
      p->tick(now_ticks);
  }
  proto_dispatch_leave();
}

void net_proto_reset(void) {
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->reset)
      p->reset();
  }
  proto_dispatch_leave();
}

void net_proto_selftest(void) {
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->selftest)
      p->selftest();
  }
  proto_dispatch_leave();
}

int net_proto_ipv6_available(void) {
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->send6)
      return 1;
  }
  return 0;
}

void net_proto_ipv6_send(struct in6_addr_k dst, u8 next_header,
                         const void *payload, usize size) {
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->send6) {
      p->send6(dst, next_header, payload, size);
      break;
    }
  }
  proto_dispatch_leave();
}

void net_proto_icmp6_unreach(struct in6_addr_k dst, u8 code, const void *quoted,
                             usize quoted_len) {
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->icmp6_unreach) {
      p->icmp6_unreach(dst, code, quoted, quoted_len);
      break;
    }
  }
  proto_dispatch_leave();
}

void ndp_dispatch_receive(struct in6_addr_k src, struct in6_addr_k dst, u8 type,
                          const void *data, usize size) {
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->icmp6) {
      p->icmp6(src, dst, type, data, size);
      break;
    }
  }
  proto_dispatch_leave();
}

int ndp_dispatch_resolve(struct in6_addr_k ip, struct mac_addr *mac) {
  int rc = 0;
  proto_dispatch_enter();
  for (struct net_proto *p = proto_list; p; p = p->next) {
    if (p->resolve6) {
      rc = p->resolve6(ip, mac);
      break;
    }
  }
  proto_dispatch_leave();
  return rc;
}
