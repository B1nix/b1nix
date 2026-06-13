/* M51: Wayland clipboard (wl_data_device selection) round-trip. Opens two
 * connections to displayd: A owns the selection (a data_source), B reads it
 * back through a pipe. Exercises set_selection -> data_offer -> receive ->
 * data_source.send fd forwarding. */
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK_PATH "/run/wayland-0"
#define CLIP "b1nix-clip"

static void mark(const char *s) { write(1, s, strlen(s)); }
static int fail(const char *s) {
  mark(s);
  return 1;
}

struct hdr {
  uint32_t object;
  uint16_t opcode;
  uint16_t size;
};
struct conn {
  int fd;
  uint8_t buf[1024];
  unsigned len;
};
struct ev {
  uint32_t object;
  uint16_t opcode;
  uint32_t args[32];
  unsigned nargs;
  int fd; /* received via SCM_RIGHTS, or -1 */
};

static int req(int fd, uint32_t obj, uint16_t op, const uint32_t *a,
               unsigned n) {
  uint8_t b[256];
  struct hdr h = {obj, op, (uint16_t)(sizeof(h) + n * 4)};
  memcpy(b, &h, sizeof(h));
  memcpy(b + sizeof(h), a, n * 4);
  return send(fd, b, h.size, 0) == h.size ? 0 : -1;
}

/* bind global `name` (version `ver`) to `new_id` on the registry (object 2). */
static int bind_global(int fd, uint32_t name, const char *iface, uint32_t ver,
                       uint32_t new_id) {
  uint32_t a[32];
  unsigned len = (unsigned)strlen(iface) + 1, nt = (len + 3) / 4;
  a[0] = name;
  a[1] = len;
  memset(&a[2], 0, nt * 4);
  memcpy(&a[2], iface, len);
  a[2 + nt] = ver;
  a[3 + nt] = new_id;
  return req(fd, 2, 0, a, 4 + nt);
}

static int req_string_fd(int fd, uint32_t obj, uint16_t op, const char *text,
                         int passed) {
  uint8_t b[256];
  char ctl[CMSG_SPACE(sizeof(int))];
  uint32_t a[32];
  unsigned len = (unsigned)strlen(text) + 1, nt = (len + 3) / 4;
  a[0] = len;
  memset(&a[1], 0, nt * 4);
  memcpy(&a[1], text, len);
  struct hdr h = {obj, op, (uint16_t)(sizeof(h) + (1 + nt) * 4)};
  memcpy(b, &h, sizeof(h));
  memcpy(b + sizeof(h), a, (1 + nt) * 4);
  struct iovec iov = {b, h.size};
  struct msghdr m;
  memset(&m, 0, sizeof(m));
  memset(ctl, 0, sizeof(ctl));
  m.msg_iov = &iov;
  m.msg_iovlen = 1;
  m.msg_control = ctl;
  m.msg_controllen = sizeof(ctl);
  struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
  cm->cmsg_level = SOL_SOCKET;
  cm->cmsg_type = SCM_RIGHTS;
  cm->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cm), &passed, sizeof(passed));
  return sendmsg(fd, &m, 0) == h.size ? 0 : -1;
}

static int next_ev(struct conn *c, struct ev *e) {
  for (;;) {
    if (c->len >= sizeof(struct hdr)) {
      struct hdr h;
      memcpy(&h, c->buf, sizeof(h));
      if (h.size < sizeof(h) || h.size > sizeof(c->buf) || (h.size & 3))
        return -1;
      if (c->len >= h.size) {
        e->object = h.object;
        e->opcode = h.opcode;
        e->nargs = (h.size - sizeof(h)) / 4;
        memcpy(e->args, c->buf + sizeof(h), e->nargs * 4);
        memmove(c->buf, c->buf + h.size, c->len - h.size);
        c->len -= h.size;
        return 1;
      }
    }
    struct pollfd pfd = {c->fd, POLLIN, 0};
    if (poll(&pfd, 1, 3000) <= 0)
      return 0;
    char ctl[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {c->buf + c->len, sizeof(c->buf) - c->len};
    struct msghdr m;
    memset(&m, 0, sizeof(m));
    memset(ctl, 0, sizeof(ctl));
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = ctl;
    m.msg_controllen = sizeof(ctl);
    ssize_t n = recvmsg(c->fd, &m, 0);
    if (n <= 0)
      return -1;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
    if (cm && cm->cmsg_type == SCM_RIGHTS)
      memcpy(&e->fd, CMSG_DATA(cm), sizeof(int));
    c->len += (unsigned)n;
  }
}

static int connect_wl(struct conn *c) {
  memset(c, 0, sizeof(*c));
  c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, SOCK_PATH);
  if (c->fd < 0 || connect(c->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    return -1;
  uint32_t reg = 2;
  return req(c->fd, 1, 1, &reg, 1); /* wl_display.get_registry -> object 2 */
}

int main(void) {
  struct conn A, B;
  int up = 0;
  for (int t = 0; t < 100; t++) {
    if (connect_wl(&A) == 0) {
      up = 1;
      break;
    }
    if (A.fd >= 0)
      close(A.fd);
    usleep(100000);
  }
  if (!up || connect_wl(&B) != 0)
    return fail("M51-GFX: fail clipboard (connect)\n");

  /* ids: ddm=10, seat=11, source=12, device=13. */
  bind_global(A.fd, 6, "wl_data_device_manager", 3, 10);
  bind_global(A.fd, 3, "wl_seat", 5, 11);
  req(A.fd, 10, 0, (uint32_t[]){12}, 1);          /* create_data_source(12) */
  {
    uint32_t a[32];
    const char *mime = "text/plain";
    unsigned len = (unsigned)strlen(mime) + 1, nt = (len + 3) / 4;
    a[0] = len;
    memset(&a[1], 0, nt * 4);
    memcpy(&a[1], mime, len);
    req(A.fd, 12, 0, a, 1 + nt);                  /* data_source.offer */
  }
  req(A.fd, 10, 1, (uint32_t[]){13, 11}, 2);      /* get_data_device(13, seat) */
  req(A.fd, 13, 1, (uint32_t[]){12, 1}, 2);       /* set_selection(source, 1) */

  /* B binds and should receive the selection offer. */
  bind_global(B.fd, 6, "wl_data_device_manager", 3, 20);
  bind_global(B.fd, 3, "wl_seat", 5, 21);
  req(B.fd, 20, 1, (uint32_t[]){23, 21}, 2);      /* get_data_device(23, seat) */

  struct ev e;
  uint32_t offer_id = 0;
  for (int i = 0; i < 64 && !offer_id; i++) {
    e.fd = -1;
    if (next_ev(&B, &e) != 1)
      break;
    if (e.object == 23 && e.opcode == 5 && e.nargs >= 1) /* selection(offer) */
      offer_id = e.args[0];
  }
  if (!offer_id)
    return fail("M51-GFX: fail clipboard (no-offer)\n");

  int pipefd[2];
  if (pipe(pipefd) != 0)
    return fail("M51-GFX: fail clipboard (pipe)\n");
  /* data_offer.receive("text/plain", pipe_w) */
  req_string_fd(B.fd, offer_id, 1, "text/plain", pipefd[1]);
  close(pipefd[1]);

  /* A receives data_source.send(mime, fd) and writes the clipboard text. */
  for (int i = 0; i < 64; i++) {
    e.fd = -1;
    if (next_ev(&A, &e) != 1)
      break;
    if (e.object == 12 && e.opcode == 1 && e.fd >= 0) { /* data_source.send */
      write(e.fd, CLIP, strlen(CLIP));
      close(e.fd);
      break;
    }
  }

  char got[32];
  int rd = (int)read(pipefd[0], got, sizeof(got) - 1);
  close(pipefd[0]);
  if (rd != (int)strlen(CLIP) || memcmp(got, CLIP, rd))
    return fail("M51-GFX: fail clipboard (data)\n");

  mark("M51-GFX: ok clipboard\n");
  return 0;
}
