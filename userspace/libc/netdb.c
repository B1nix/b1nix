#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "syscall.h"

/* ── Address text <-> binary ─────────────────────────────────────────────── */

int inet_pton(int af, const char *src, void *dst) {
  if (af != AF_INET || !src || !dst)
    return -1;
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
  unsigned char *out = (unsigned char *)dst;
  out[0] = (unsigned char)parts[0];
  out[1] = (unsigned char)parts[1];
  out[2] = (unsigned char)parts[2];
  out[3] = (unsigned char)parts[3];
  return 1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
  if (af != AF_INET || !src || !dst)
    return NULL;
  const unsigned char *b = (const unsigned char *)src;
  char tmp[16];
  int n = snprintf(tmp, sizeof(tmp), "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
  if (n < 0 || (socklen_t)n >= size)
    return NULL;
  memcpy(dst, tmp, (size_t)n + 1);
  return dst;
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

struct hostent *gethostbyname(const char *name) {
  static struct hostent he;
  static unsigned char addr[4];
  static char *addr_list[2];
  static char *aliases[1];
  static char namebuf[256];

  if (resolve_host(name, addr) != 0)
    return NULL;

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
  if (family != 0 && family != AF_INET)
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

  unsigned char addr[4];
  if (node && *node) {
    if (resolve_host(node, addr) != 0)
      return EAI_NONAME;
  } else {
    /* AI_PASSIVE: bind to any (0.0.0.0); otherwise loopback-ish default. */
    addr[0] = addr[1] = addr[2] = addr[3] = 0;
  }

  struct addrinfo *ai = malloc(sizeof(struct addrinfo));
  struct sockaddr_in *sa = malloc(sizeof(struct sockaddr_in));
  if (!ai || !sa) {
    free(ai);
    free(sa);
    return EAI_MEMORY;
  }
  memset(ai, 0, sizeof(*ai));
  memset(sa, 0, sizeof(*sa));

  sa->sin_family = AF_INET;
  sa->sin_port = htons(port);
  sa->sin_addr = (unsigned int)addr[0] | ((unsigned int)addr[1] << 8) |
                 ((unsigned int)addr[2] << 16) | ((unsigned int)addr[3] << 24);

  ai->ai_family = AF_INET;
  ai->ai_socktype = socktype ? socktype : SOCK_STREAM;
  ai->ai_protocol = 0;
  ai->ai_addrlen = sizeof(struct sockaddr_in);
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
