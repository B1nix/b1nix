#ifndef B1NIX_U_NETDB_H
#define B1NIX_U_NETDB_H

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hostent {
  char *h_name;        /* official name of host */
  char **h_aliases;    /* alias list (NULL-terminated) */
  int h_addrtype;      /* host address type (AF_INET) */
  int h_length;        /* length of address (4) */
  char **h_addr_list;  /* list of addresses (NULL-terminated) */
};
#define h_addr h_addr_list[0]

struct addrinfo {
  int ai_flags;
  int ai_family;
  int ai_socktype;
  int ai_protocol;
  socklen_t ai_addrlen;
  struct sockaddr *ai_addr;
  char *ai_canonname;
  struct addrinfo *ai_next;
};

#define AI_PASSIVE     0x0001
#define AI_CANONNAME   0x0002
#define AI_NUMERICHOST 0x0004

#define EAI_BADFLAGS  -1
#define EAI_NONAME    -2
#define EAI_AGAIN     -3
#define EAI_FAIL      -4
#define EAI_FAMILY    -6
#define EAI_SOCKTYPE  -7
#define EAI_SERVICE   -8
#define EAI_MEMORY    -10
#define EAI_SYSTEM    -11

struct hostent *gethostbyname(const char *name);
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int errcode);

#ifdef __cplusplus
}
#endif

#endif
