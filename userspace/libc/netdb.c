#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "syscall.h"

int h_errno = 0;
const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

/* ── Address text <-> binary ─────────────────────────────────────────────── */


static int parse_ipv4(const char *src, unsigned char out[4]) {
  unsigned int parts[4];
  int n = 0, val = 0, digits = 0;
  for (const char *p = src;; p++) {
    if (*p >= '0' && *p <= '9') {
      val = val * 10 + (*p - '0');
      if (val > 255) return 0;
      digits++;
    } else if (*p == '.' || *p == '\0') {
      if (!digits || n >= 4) return 0;
      parts[n++] = (unsigned int)val;
      val = 0;
      digits = 0;
      if (*p == '\0') break;
    } else {
      return 0;
    }
  }
  if (n != 4) return 0;
  out[0] = (unsigned char)parts[0];
  out[1] = (unsigned char)parts[1];
  out[2] = (unsigned char)parts[2];
  out[3] = (unsigned char)parts[3];
  return 1;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int parse_ipv6(const char *src, unsigned char out[16]) {
  unsigned short words[8];
  int wc = 0, dc = -1;
  const char *p = src;
  if (*p == ':') {
    if (p[1] != ':') return 0;
    dc = 0;
    p += 2;
  }
  while (*p) {
    if (wc >= 8) return 0;
    if (*p == ':') {
      if (dc != -1) return 0;
      dc = wc;
      p++;
      if (*p == ':') p++;
      continue;
    }
    unsigned int val = 0;
    int digits = 0;
    const char *seg = p;
    while (*p) {
      int hv = hexval(*p);
      if (hv < 0) break;
      val = (val << 4) | (unsigned int)hv;
      if (++digits > 4) return 0;
      p++;
    }
    if (digits == 0) return 0;
    if (*p == '.') {
      if (wc > 6) return 0;
      unsigned char v4[4];
      if (!parse_ipv4(seg, v4)) return 0;
      words[wc++] = (unsigned short)(((unsigned short)v4[0] << 8) | v4[1]);
      words[wc++] = (unsigned short)(((unsigned short)v4[2] << 8) | v4[3]);
      p += strlen(p);
      break;
    }
    words[wc++] = (unsigned short)val;
    if (*p == ':') p++;
    else if (*p != '\0') return 0;
  }
  if (dc != -1) {
    int zeros = 8 - wc;
    if (zeros <= 0) return 0;
    for (int i = wc - 1; i >= dc; i--) words[i + zeros] = words[i];
    for (int i = 0; i < zeros; i++) words[dc + i] = 0;
    wc = 8;
  }
  if (wc != 8) return 0;
  for (int i = 0; i < 8; i++) {
    out[2 * i] = (unsigned char)(words[i] >> 8);
    out[2 * i + 1] = (unsigned char)(words[i] & 0xff);
  }
  return 1;
}

int inet_pton(int af, const char *src, void *dst) {
  if (!src || !dst) return -1;
  if (af == AF_INET) return parse_ipv4(src, (unsigned char *)dst);
  if (af == AF_INET6) return parse_ipv6(src, (unsigned char *)dst);
  return -1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
  if (!src || !dst) return NULL;
  if (af == AF_INET) {
    const unsigned char *b = (const unsigned char *)src;
    char tmp[16];
    int n = snprintf(tmp, sizeof(tmp), "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
    if (n < 0 || (socklen_t)n >= size) return NULL;
    memcpy(dst, tmp, (size_t)n + 1);
    return dst;
  }
  if (af == AF_INET6) {
    const unsigned char *b = (const unsigned char *)src;
    unsigned g[8];
    for (int k = 0; k < 8; k++)
      g[k] = ((unsigned)b[2 * k] << 8) | b[2 * k + 1];
    /* RFC 5952: collapse the longest run (>= 2) of zero groups to "::". */
    int best = -1, bestlen = 0, cur = -1, curlen = 0;
    for (int k = 0; k < 8; k++) {
      if (g[k] == 0) {
        if (cur < 0) { cur = k; curlen = 1; } else { curlen++; }
        if (curlen > bestlen) { best = cur; bestlen = curlen; }
      } else {
        cur = -1;
        curlen = 0;
      }
    }
    if (bestlen < 2) best = -1;
    char tmp[INET6_ADDRSTRLEN];
    int pos = 0, k = 0;
    while (k < 8) {
      if (k == best) {
        tmp[pos++] = ':';
        k += bestlen;
        if (k == 8) tmp[pos++] = ':';
        continue;
      }
      if (k > 0) tmp[pos++] = ':';
      pos += snprintf(tmp + pos, sizeof(tmp) - (size_t)pos, "%x", g[k]);
      k++;
    }
    tmp[pos] = '\0';
    if ((socklen_t)pos >= size) return NULL;
    memcpy(dst, tmp, (size_t)pos + 1);
    return dst;
  }
  return NULL;
}

int inet_aton(const char *cp, struct in_addr *inp) {
  unsigned char b[4];
  if (inet_pton(AF_INET, cp, b) != 1)
    return 0;
  if (inp)
    inp->s_addr = (unsigned int)b[0] | ((unsigned int)b[1] << 8) |
                  ((unsigned int)b[2] << 16) | ((unsigned int)b[3] << 24);
  return 1;
}

unsigned int inet_addr(const char *cp) {
  struct in_addr a;
  if (inet_aton(cp, &a) == 0)
    return 0xffffffffu; /* INADDR_NONE */
  return a.s_addr;
}

char *inet_ntoa(struct in_addr in) {
  static char buf[16];
  unsigned char b[4];
  b[0] = (unsigned char)(in.s_addr);
  b[1] = (unsigned char)(in.s_addr >> 8);
  b[2] = (unsigned char)(in.s_addr >> 16);
  b[3] = (unsigned char)(in.s_addr >> 24);
  inet_ntop(AF_INET, b, buf, sizeof(buf));
  return buf;
}

/* ── Name resolution ─────────────────────────────────────────────────────── */

/* Resolve a host (numeric or name) to a 4-byte network-order address. Numeric
 * dotted-quads are handled locally; names go through the kernel DNS client via
 * SYS_NET_DNS(name, out4). Returns 0 on success. */
static int resolve_host(const char *name, unsigned char out[4]) {
  if (!name || !*name)
    return -1;
  if (inet_pton(AF_INET, name, out) == 1)
    return 0;
  long rc = syscall(SYS_NET_DNS, name, out);
  return rc == 0 ? 0 : -1;
}

static int resolve_host6(const char *name, unsigned char out[16]) {
  if (!name || !*name) return -1;
  if (inet_pton(AF_INET6, name, out) == 1) return 0;
  if (strcmp(name, "localhost") == 0) {
    memcpy(out, in6addr_loopback.s6_addr, 16);
    return 0;
  }
  return -1;
}

struct hostent *gethostbyname(const char *name) {
  static struct hostent he;
  static unsigned char addr[4];
  static char *addr_list[2];
  static char *aliases[1];
  static char namebuf[256];

  if (resolve_host(name, addr) != 0) {
    h_errno = HOST_NOT_FOUND;
    return NULL;
  }

  size_t nl = strlen(name);
  if (nl >= sizeof(namebuf)) nl = sizeof(namebuf) - 1;
  memcpy(namebuf, name, nl);
  namebuf[nl] = '\0';

  addr_list[0] = (char *)addr;
  addr_list[1] = NULL;
  aliases[0] = NULL;

  he.h_name = namebuf;
  he.h_aliases = aliases;
  he.h_addrtype = AF_INET;
  he.h_length = 4;
  he.h_addr_list = addr_list;
  return &he;
}

const char *gai_strerror(int errcode) {
  switch (errcode) {
  case 0:            return "Success";
  case EAI_BADFLAGS: return "Bad value for ai_flags";
  case EAI_NONAME:   return "Name or service not known";
  case EAI_AGAIN:    return "Temporary failure in name resolution";
  case EAI_FAIL:     return "Non-recoverable failure in name resolution";
  case EAI_FAMILY:   return "ai_family not supported";
  case EAI_SOCKTYPE: return "ai_socktype not supported";
  case EAI_SERVICE:  return "Service not supported for ai_socktype";
  case EAI_MEMORY:   return "Memory allocation failure";
  default:           return "Unknown error";
  }
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
  if (!res)
    return EAI_FAIL;
  *res = NULL;

  int family = hints ? hints->ai_family : 0;
  if (family != 0 && family != AF_INET && family != AF_INET6)
    return EAI_FAMILY;
  int socktype = hints ? hints->ai_socktype : 0;

  unsigned short port = 0;
  if (service && *service) {
    /* numeric port only (no /etc/services) */
    int p = 0;
    for (const char *s = service; *s; s++) {
      if (*s < '0' || *s > '9') return EAI_SERVICE;
      p = p * 10 + (*s - '0');
    }
    port = (unsigned short)p;
  }

  struct addrinfo *ai = malloc(sizeof(struct addrinfo));
  void *sa = NULL;
  if (!ai) {
    free(ai);
    return EAI_MEMORY;
  }
  memset(ai, 0, sizeof(*ai));
  if (family == AF_INET6) {
    unsigned char addr6[16];
    if (node && *node) {
      if (resolve_host6(node, addr6) != 0) {
        free(ai);
        return EAI_NONAME;
      }
    } else {
      memcpy(addr6, in6addr_any.s6_addr, 16);
    }
    struct sockaddr_in6 *sa6 = malloc(sizeof(struct sockaddr_in6));
    if (!sa6) {
      free(ai);
      return EAI_MEMORY;
    }
    memset(sa6, 0, sizeof(*sa6));
    sa6->sin6_family = AF_INET6;
    sa6->sin6_port = htons(port);
    memcpy(sa6->sin6_addr.s6_addr, addr6, 16);
    ai->ai_family = AF_INET6;
    ai->ai_addrlen = sizeof(struct sockaddr_in6);
    sa = sa6;
  } else {
    unsigned char addr[4];
    if (node && *node) {
      if (resolve_host(node, addr) != 0) {
        free(ai);
        return EAI_NONAME;
      }
    } else {
      addr[0] = addr[1] = addr[2] = addr[3] = 0;
    }
    struct sockaddr_in *sa4 = malloc(sizeof(struct sockaddr_in));
    if (!sa4) {
      free(ai);
      return EAI_MEMORY;
    }
    memset(sa4, 0, sizeof(*sa4));
    sa4->sin_family = AF_INET;
    sa4->sin_port = htons(port);
    sa4->sin_addr.s_addr = (unsigned int)addr[0] | ((unsigned int)addr[1] << 8) |
                           ((unsigned int)addr[2] << 16) | ((unsigned int)addr[3] << 24);
    ai->ai_family = AF_INET;
    ai->ai_addrlen = sizeof(struct sockaddr_in);
    sa = sa4;
  }

  ai->ai_socktype = socktype ? socktype : SOCK_STREAM;
  ai->ai_protocol = 0;
  ai->ai_addr = (struct sockaddr *)sa;
  ai->ai_canonname = NULL;
  ai->ai_next = NULL;

  *res = ai;
  return 0;
}

void freeaddrinfo(struct addrinfo *res) {
  while (res) {
    struct addrinfo *next = res->ai_next;
    free(res->ai_addr);
    free(res);
    res = next;
  }
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host,
                socklen_t hostlen, char *serv, socklen_t servlen, int flags) {
  (void)salen;
  (void)flags;
  if (!sa) return EAI_FAIL;

  unsigned short port = 0;
  if (sa->sa_family == AF_INET) {
    const struct sockaddr_in *s4 = (const struct sockaddr_in *)sa;
    if (host && hostlen &&
        !inet_ntop(AF_INET, &s4->sin_addr, host, hostlen))
      return EAI_OVERFLOW;
    port = ntohs(s4->sin_port);
  } else if (sa->sa_family == AF_INET6) {
    const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)sa;
    if (host && hostlen &&
        !inet_ntop(AF_INET6, &s6->sin6_addr, host, hostlen))
      return EAI_OVERFLOW;
    port = ntohs(s6->sin6_port);
  } else {
    return EAI_FAMILY;
  }

  if (serv && servlen) {
    char rev[8];
    int ri = 0;
    unsigned short p = port;
    if (p == 0) rev[ri++] = '0';
    while (p) { rev[ri++] = (char)('0' + (p % 10)); p /= 10; }
    if ((socklen_t)(ri + 1) > servlen) return EAI_OVERFLOW;
    int n = 0;
    while (ri) serv[n++] = rev[--ri];
    serv[n] = '\0';
  }
  return 0;
}
