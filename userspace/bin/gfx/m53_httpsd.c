/* m53_httpsd — a minimal HTTPS (TLS 1.2) server for the M53 web-access smoke.
 * It serves one fixed styled HTML page over TLS on 127.0.0.1:8443, using the
 * b1nix M32 loopback PKI (/etc/tls-test/{server-cert,server-key}.pem). NetSurf
 * then fetches https://127.0.0.1:8443/ via libcurl (mbedTLS backend), verifying
 * the server certificate against /etc/tls-test/ca.pem (SAN IP:127.0.0.1) — a
 * real TLS handshake + HTTP-over-TLS, no fakes. Serves in a loop until killed.
 *
 * The mbedTLS server flow mirrors m32-nettool's tls-server, generalised to a
 * persistent multi-connection loop serving a full HTML body. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

static int read_pem(const char *path, unsigned char *buf, size_t cap,
                    size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (f == NULL)
    return -1;
  size_t n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = '\0'; /* mbedTLS PEM parse wants a NUL-terminated buffer */
  *out_len = n + 1;
  return 0;
}

static int ssl_bio_send(void *ctx, const unsigned char *b, size_t l) {
  int fd = *(int *)ctx;
  ssize_t n = write(fd, b, l);
  return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)n;
}
static int ssl_bio_recv(void *ctx, unsigned char *b, size_t l) {
  int fd = *(int *)ctx;
  ssize_t n = read(fd, b, l);
  return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)n;
}

static const char body[] =
    "<!DOCTYPE html><html><head><title>b1nix https</title>\n"
    "<style>body{background:#ffffff;color:#102040;font-family:sans-serif;"
    "margin:16px}h1{color:#108040}.card{background:#e0f8e8;"
    "border:3px solid #208050;padding:10px}</style></head>\n"
    "<body><h1>NetSurf fetched this over HTTPS</h1>\n"
    "<p>This page was served over a real TLS 1.2 connection and fetched by "
    "NetSurf through libcurl (mbedTLS) on b1nix, with the server certificate "
    "verified against the test CA.</p>\n"
    "<div class=\"card\"><p>A styled card delivered over TLS.</p></div>\n"
    "<p>Encrypted line one.</p>\n"
    "<p>Encrypted line two.</p>\n"
    "<p>Encrypted line three.</p>\n"
    "</body></html>\n";

int main(void) {
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) { emit("M53-HTTPSD: socket-fail\n"); return 1; }
  int one = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(8443);
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(lfd, 8) < 0) {
    emit("M53-HTTPSD: bind-fail\n");
    return 1;
  }

  unsigned char certbuf[4096], keybuf[2048];
  size_t certlen = 0, keylen = 0;
  if (read_pem("/etc/tls-test/server-cert.pem", certbuf, sizeof(certbuf),
               &certlen) ||
      read_pem("/etc/tls-test/server-key.pem", keybuf, sizeof(keybuf),
               &keylen)) {
    emit("M53-HTTPSD: cert-load-fail\n");
    return 1;
  }

  mbedtls_x509_crt srvcert;
  mbedtls_pk_context pkey;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_ssl_config conf;
  mbedtls_x509_crt_init(&srvcert);
  mbedtls_pk_init(&pkey);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_ssl_config_init(&conf);

  const char *pers = "b1nix-httpsd";
  if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                            (const unsigned char *)pers, strlen(pers)) != 0 ||
      mbedtls_x509_crt_parse(&srvcert, certbuf, certlen) != 0 ||
      mbedtls_pk_parse_key(&pkey, keybuf, keylen, NULL, 0,
                           mbedtls_ctr_drbg_random, &ctr_drbg) != 0 ||
      mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER,
                                  MBEDTLS_SSL_TRANSPORT_STREAM,
                                  MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
    emit("M53-HTTPSD: tls-setup-fail\n");
    return 1;
  }
  mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
  if (mbedtls_ssl_conf_own_cert(&conf, &srvcert, &pkey) != 0) {
    emit("M53-HTTPSD: own-cert-fail\n");
    return 1;
  }

  char resp[4096];
  int hlen = snprintf(resp, sizeof(resp),
                      "HTTP/1.0 200 OK\r\n"
                      "Content-Type: text/html; charset=utf-8\r\n"
                      "Content-Length: %u\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      (unsigned)(sizeof(body) - 1));

  emit("M53-HTTPSD: ready\n");

  for (;;) {
    int cfd = accept(lfd, 0, 0);
    if (cfd < 0)
      continue;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_init(&ssl);
    if (mbedtls_ssl_setup(&ssl, &conf) != 0) {
      mbedtls_ssl_free(&ssl);
      close(cfd);
      continue;
    }
    mbedtls_ssl_set_bio(&ssl, &cfd, ssl_bio_send, ssl_bio_recv, NULL);
    int rc, ok = 1;
    while ((rc = mbedtls_ssl_handshake(&ssl)) != 0) {
      if (rc != MBEDTLS_ERR_SSL_WANT_READ &&
          rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
        ok = 0;
        break;
      }
    }
    if (ok) {
      unsigned char rbuf[1024];
      mbedtls_ssl_read(&ssl, rbuf, sizeof(rbuf));
      mbedtls_ssl_write(&ssl, (const unsigned char *)resp, (size_t)hlen);
      size_t off = 0, blen = sizeof(body) - 1;
      while (off < blen) {
        int n = mbedtls_ssl_write(&ssl, (const unsigned char *)body + off,
                                  blen - off);
        if (n <= 0)
          break;
        off += (size_t)n;
      }
      mbedtls_ssl_close_notify(&ssl);
    }
    mbedtls_ssl_free(&ssl);
    close(cfd);
  }
  return 0;
}
