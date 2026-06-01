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

struct servent {
  char *s_name;        /* official service name */
  char **s_aliases;    /* alias list */
  int s_port;          /* port number (network byte order) */
  char *s_proto;       /* protocol to use */
};

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
#define EAI_OVERFLOW  -12

/* getnameinfo() flags */
#define NI_NUMERICHOST 0x01
#define NI_NUMERICSERV 0x02
#define NI_NAMEREQD    0x04
#define NI_DGRAM       0x10
#define NI_MAXHOST     1025
#define NI_MAXSERV     32

#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4
#define NO_ADDRESS     NO_DATA

extern int h_errno;

struct hostent *gethostbyname(const char *name);
struct servent *getservbyname(const char *name, const char *proto);
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int errcode);
int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host,
                socklen_t hostlen, char *serv, socklen_t servlen, int flags);

#ifdef __cplusplus
}
#endif

#endif
