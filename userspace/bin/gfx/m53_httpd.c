/* m53_httpd — a minimal HTTP/1.0 server for the M53 "web access" smoke. It
 * serves one fixed, styled HTML page (heading, body text, a bordered/coloured
 * box, and an inline data: image) on 0.0.0.0:8080 for any GET. NetSurf then
 * fetches http://127.0.0.1:8080/ over a real TCP connection through libcurl,
 * proving the browser has genuine web access (sockets + TCP + HTTP), not just
 * the local file:// path.
 *
 * Runs until killed. Prints "M53-HTTPD: ready" once listening so the parent can
 * proceed. No fakes: this is a real socket server speaking real HTTP. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

/* A 16x16 red/blue checker PNG, base64 — small enough to inline as a data URI
 * so the whole page is one HTTP response (no second request). */
static const char body[] =
    "<!DOCTYPE html><html><head><title>b1nix web</title>\n"
    "<style>body{background:#ffffff;color:#102040;font-family:sans-serif;"
    "margin:16px}h1{color:#1060c0}.card{background:#e0e8f8;"
    "border:3px solid #4060a0;padding:10px}</style></head>\n"
    "<body><h1>NetSurf fetched this over HTTP</h1>\n"
    "<p>This page was served by an in-VM HTTP server and fetched by NetSurf "
    "over a real TCP connection through libcurl on b1nix.</p>\n"
    "<div class=\"card\"><p>A styled card served from the network.</p>\n"
    "<img src=\"data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAIAAACQkWg2AAAAOUlEQVR4nGP8z4AAjAxIgInh"
    "PwMW8B+nDFZTGRgYGP8jaWBkxKqGCV0Aq1IcBmA1FacBeJ3KgAYAph0Lg0n0bL0AAAAASUVO"
    "RK5CYII=\" width=\"32\" height=\"32\" alt=\"img\"></div>\n"
    "<p>Line one of network body text.</p>\n"
    "<p>Line two of network body text.</p>\n"
    "<p>Line three of network body text.</p>\n"
    "</body></html>\n";

int main(void) {
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) {
    emit("M53-HTTPD: socket-fail\n");
    return 1;
  }
  int one = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(8080);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    emit("M53-HTTPD: bind-fail\n");
    return 1;
  }
  if (listen(lfd, 8) < 0) {
    emit("M53-HTTPD: listen-fail\n");
    return 1;
  }
  emit("M53-HTTPD: ready\n");

  /* Pre-format the response (HTTP/1.0 + Content-Length + Connection: close). */
  char resp[4096];
  int hlen = snprintf(resp, sizeof(resp),
                      "HTTP/1.0 200 OK\r\n"
                      "Content-Type: text/html; charset=utf-8\r\n"
                      "Content-Length: %u\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      (unsigned)(sizeof(body) - 1));
  for (;;) {
    int cfd = accept(lfd, 0, 0);
    if (cfd < 0)
      continue;
    /* Drain the request line/headers (we serve the same page for any GET). */
    char req[1024];
    (void)read(cfd, req, sizeof(req));
    /* Write headers then body. */
    write(cfd, resp, (size_t)hlen);
    size_t off = 0, blen = sizeof(body) - 1;
    while (off < blen) {
      ssize_t n = write(cfd, body + off, blen - off);
      if (n <= 0)
        break;
      off += (size_t)n;
    }
    close(cfd);
  }
  return 0;
}
