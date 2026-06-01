#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef B1NIX_HAVE_MBEDTLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#endif

static int connect_tcp(const char *host, const char *port) {
  struct addrinfo hints, *res = 0;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  int gai = getaddrinfo(host, port, &hints, &res);
  if (gai != 0 || !res) {
    printf("m32-nettool: resolve failed: %s\n", gai_strerror(gai));
    return -1;
  }

  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd;
}

static int http_get(int argc, char **argv) {
  if (argc < 4) {
    printf("usage: m32-nettool http-get HOST PORT PATH\n");
    return 1;
  }

  int fd = connect_tcp(argv[1], argv[2]);
  if (fd < 0) {
    printf("m32-nettool: connect failed\n");
    return 1;
  }

  char req[512];
  int n = snprintf(req, sizeof(req),
                   "GET %s HTTP/1.0\r\n"
                   "Host: %s\r\n"
                   "User-Agent: b1nix-m32-nettool/1.0\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   argv[3], argv[1]);
  send(fd, req, (size_t)n, 0);

  char buf[512];
  while ((n = (int)recv(fd, buf, sizeof(buf), 0)) > 0) {
    write(1, buf, (size_t)n);
  }
  close(fd);
  return 0;
}

static int tcp_echo_server(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: m32-nettool tcp-echo PORT\n");
    return 1;
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)atoi(argv[1]));
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(fd, 1) < 0) {
    close(fd);
    return 1;
  }

  int cfd = accept(fd, 0, 0);
  if (cfd < 0) {
    close(fd);
    return 1;
  }

  char buf[256];
  int n = (int)recv(cfd, buf, sizeof(buf), 0);
  if (n > 0) send(cfd, buf, (size_t)n, 0);
  close(cfd);
  close(fd);
  return 0;
}

#ifdef B1NIX_HAVE_MBEDTLS
/* Read a PEM file into buf and NUL-terminate it. mbedTLS PEM parsers require
 * the length to include the terminating NUL, so *out_len is bytes + 1. */
static int read_pem(const char *path, unsigned char *buf, size_t cap,
                    size_t *out_len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  size_t total = 0;
  for (;;) {
    if (total >= cap - 1) {
      close(fd);
      return -1;
    }
    int r = (int)read(fd, buf + total, cap - 1 - total);
    if (r < 0) {
      close(fd);
      return -1;
    }
    if (r == 0) break;
    total += (size_t)r;
  }
  close(fd);
  buf[total] = '\0';
  *out_len = total + 1;
  return 0;
}

static int ssl_bio_send(void *ctx, const unsigned char *b, size_t l) {
  int fd = *(int *)ctx;
  int r = (int)send(fd, b, l, 0);
  if (r == 0) return MBEDTLS_ERR_SSL_WANT_WRITE; /* window full: retry later */
  if (r < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  return r;
}

static int ssl_bio_recv(void *ctx, unsigned char *b, size_t l) {
  int fd = *(int *)ctx;
  int r = (int)recv(fd, b, l, 0);
  if (r < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  return r;
}

/* Minimal single-shot loopback HTTPS server: bind+listen on 127.0.0.1:PORT,
 * perform one TLS handshake presenting the embedded test cert, reply with a
 * tiny HTTP body, and exit. Used by the M32 curl-TLS loopback smoke. */
static int tls_server(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: m32-nettool tls-server PORT\n");
    return 1;
  }
  int port = atoi(argv[1]);

  /* Bind and listen FIRST so a client's connect() never races server init. */
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) return 1;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(lfd, 1) < 0) {
    close(lfd);
    return 1;
  }

  unsigned char certbuf[4096], keybuf[2048];
  size_t certlen = 0, keylen = 0;
  if (read_pem("/etc/tls-test/server-cert.pem", certbuf, sizeof(certbuf),
               &certlen) ||
      read_pem("/etc/tls-test/server-key.pem", keybuf, sizeof(keybuf),
               &keylen)) {
    fprintf(stderr, "tls-server: cert/key load failed\n");
    close(lfd);
    return 1;
  }

  mbedtls_x509_crt srvcert;
  mbedtls_pk_context pkey;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_ssl_config conf;
  mbedtls_ssl_context ssl;
  mbedtls_x509_crt_init(&srvcert);
  mbedtls_pk_init(&pkey);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_ssl_config_init(&conf);
  mbedtls_ssl_init(&ssl);

  const char *pers = "b1nix-tls-server";
  int rc;
  if ((rc = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                  (const unsigned char *)pers,
                                  strlen(pers))) != 0) {
    fprintf(stderr, "tls-server: drbg seed failed -0x%x\n", -rc);
    close(lfd);
    return 1;
  }
  if ((rc = mbedtls_x509_crt_parse(&srvcert, certbuf, certlen)) != 0) {
    fprintf(stderr, "tls-server: cert parse failed -0x%x\n", -rc);
    close(lfd);
    return 1;
  }
  if ((rc = mbedtls_pk_parse_key(&pkey, keybuf, keylen, NULL, 0,
                                 mbedtls_ctr_drbg_random, &ctr_drbg)) != 0) {
    fprintf(stderr, "tls-server: key parse failed -0x%x\n", -rc);
    close(lfd);
    return 1;
  }
  if ((rc = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
    fprintf(stderr, "tls-server: config defaults failed -0x%x\n", -rc);
    close(lfd);
    return 1;
  }
  /* Pin TLS 1.2: a single ECDHE-ECDSA flight keeps the handshake small for
   * the in-kernel loopback path and avoids the heavier TLS 1.3 server state. */
  mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
  if ((rc = mbedtls_ssl_conf_own_cert(&conf, &srvcert, &pkey)) != 0) {
    fprintf(stderr, "tls-server: own cert failed -0x%x\n", -rc);
    close(lfd);
    return 1;
  }
  if ((rc = mbedtls_ssl_setup(&ssl, &conf)) != 0) {
    fprintf(stderr, "tls-server: ssl setup failed -0x%x\n", -rc);
    close(lfd);
    return 1;
  }

  int cfd = accept(lfd, 0, 0);
  if (cfd < 0) {
    close(lfd);
    return 1;
  }
  mbedtls_ssl_set_bio(&ssl, &cfd, ssl_bio_send, ssl_bio_recv, NULL);

  while ((rc = mbedtls_ssl_handshake(&ssl)) != 0) {
    if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
      fprintf(stderr, "tls-server: handshake failed -0x%x\n", -rc);
      close(cfd);
      close(lfd);
      return 1;
    }
  }

  unsigned char rbuf[1024];
  mbedtls_ssl_read(&ssl, rbuf, sizeof(rbuf));
  const char *resp = "HTTP/1.0 200 OK\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: 15\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "tls-loopback-ok";
  mbedtls_ssl_write(&ssl, (const unsigned char *)resp, strlen(resp));
  mbedtls_ssl_close_notify(&ssl);

  close(cfd);
  close(lfd);
  mbedtls_ssl_free(&ssl);
  mbedtls_ssl_config_free(&conf);
  mbedtls_x509_crt_free(&srvcert);
  mbedtls_pk_free(&pkey);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
  return 0;
}
#endif /* B1NIX_HAVE_MBEDTLS */

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: m32-nettool http-get|tcp-echo|tls-server ...\n");
    return 1;
  }
  if (strcmp(argv[1], "http-get") == 0) return http_get(argc - 1, argv + 1);
  if (strcmp(argv[1], "tcp-echo") == 0) return tcp_echo_server(argc - 1, argv + 1);
#ifdef B1NIX_HAVE_MBEDTLS
  if (strcmp(argv[1], "tls-server") == 0) return tls_server(argc - 1, argv + 1);
#endif
  printf("m32-nettool: unknown command '%s'\n", argv[1]);
  return 1;
}
