/* M36 diagnostic self-test: exercises the GDB serial stub's protocol engine
 * (without a live host) and the ftrace function tracer. Test mode only. Emits
 * M36-GDB / M36-FTRACE markers consumed by tests/smoke.sh. */

#include <b1nix/arch_x86_64.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/ftrace.h>
#include <b1nix/gdbstub.h>
#include <b1nix/klog.h>
#include <stdio.h>
#include <string.h>

/* Demo functions traced by ftrace (compiled with -finstrument-functions). */
int ftrace_demo_work(int x);

/* ── helpers ── */
static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Decode a little-endian 8-byte register from `hex` (16 chars). */
static u64 decode_le64(const char *hex) {
  u64 v = 0;
  for (int b = 0; b < 8; b++) {
    int hi = hexval(hex[2 * b]), lo = hexval(hex[2 * b + 1]);
    v |= (u64)((hi << 4) | lo) << (8 * b);
  }
  return v;
}

/* A known memory probe for the 'm' packet test. */
static const u8 m36_probe[4] = {0xde, 0xad, 0xbe, 0xef};

/* ── in-memory transport for the framing round-trip ── */
struct membuf {
  const char *in;
  usize inpos, inlen;
  char *out;
  usize outpos, outcap;
};
static int mem_getc(void *c) {
  struct membuf *m = c;
  return m->inpos < m->inlen ? (int)(unsigned char)m->in[m->inpos++] : -1;
}
static void mem_putc(void *c, char ch) {
  struct membuf *m = c;
  if (m->outpos < m->outcap)
    m->out[m->outpos++] = ch;
}

static void gdb_selftest(void) {
  console_write("M36-GDB: start\n");

  struct interrupt_frame f;
  memset(&f, 0, sizeof(f));
  f.rip = 0x00000000001234abULL;
  f.rax = 0xcafef00dULL;

  char resp[1200];
  int resume = 0;

  /* ? → stop reply S05 */
  gdb_handle_packet("?", resp, sizeof(resp), &f, &resume);
  if (strcmp(resp, "S05") == 0)
    console_write("M36-GDB: ok stop-reply\n");
  else
    console_write("M36-GDB: fail stop-reply\n");

  /* g → read-all-registers; rip is register index 16 (offset 256 hex chars). */
  gdb_handle_packet("g", resp, sizeof(resp), &f, &resume);
  u64 rip = decode_le64(resp + 16 * 16);
  u64 rax = decode_le64(resp + 0);
  if (rip == f.rip && rax == f.rax)
    console_write("M36-GDB: ok read-regs\n");
  else
    console_write("M36-GDB: fail read-regs\n");

  /* m <addr>,4 → read memory; expect the probe bytes back as hex. */
  char mpkt[64];
  snprintf(mpkt, sizeof(mpkt), "m%lx,4", (unsigned long)(usize)m36_probe);
  gdb_handle_packet(mpkt, resp, sizeof(resp), &f, &resume);
  if (strcmp(resp, "deadbeef") == 0)
    console_write("M36-GDB: ok read-mem\n");
  else
    console_write("M36-GDB: fail read-mem\n");

  /* Framing round-trip: feed "$?#3f" through gdb_recv_packet over a memory
   * transport, confirm it decodes "?" and acks '+', then that gdb_send_packet
   * frames a payload with the right checksum. */
  {
    /* checksum of "?" = 0x3f */
    const char *input = "$?#3f";
    char outbuf[32];
    struct membuf mb = {input, 0, strlen(input), outbuf, 0, sizeof(outbuf)};
    struct gdb_transport t = {mem_getc, mem_putc, &mb};
    char pkt[16];
    int len = gdb_recv_packet(&t, pkt, sizeof(pkt));
    int ack_ok = (mb.outpos >= 1 && outbuf[0] == '+');
    mb.outpos = 0;
    gdb_send_packet(&t, "OK");
    /* "OK" checksum = ('O'=0x4f) + ('K'=0x4b) = 0x9a → "$OK#9a" */
    int send_ok = (mb.outpos == 6 && outbuf[0] == '$' && outbuf[1] == 'O' &&
                   outbuf[2] == 'K' && outbuf[3] == '#' && outbuf[4] == '9' &&
                   outbuf[5] == 'a');
    if (len == 1 && pkt[0] == '?' && ack_ok && send_ok)
      console_write("M36-GDB: ok framing\n");
    else
      console_write("M36-GDB: fail framing\n");
  }

  console_write("M36-GDB: done\n");
}

static void ftrace_selftest(void) {
  console_write("M36-FTRACE: start\n");

  ftrace_reset();
  ftrace_enable();
  volatile int r = ftrace_demo_work(21);
  ftrace_disable();
  (void)r;

  /* Expect at least one ENTER and one EXIT for ftrace_demo_work, and that its
   * address symbolises back to the function name. */
  int saw_enter = 0, saw_exit = 0, saw_work = 0;
  usize n = ftrace_count();
  for (usize i = 0; i < n; i++) {
    const struct ftrace_event *e = ftrace_get(i);
    if (!e)
      continue;
    if (e->type == FTRACE_ENTER)
      saw_enter = 1;
    if (e->type == FTRACE_EXIT)
      saw_exit = 1;
    u64 off = 0;
    const char *name = ksym_lookup(e->addr, &off);
    if (name && strcmp(name, "ftrace_demo_work") == 0)
      saw_work = 1;
  }

  if (n >= 2 && saw_enter && saw_exit)
    console_write("M36-FTRACE: ok capture\n");
  else
    console_write("M36-FTRACE: fail capture\n");

  if (saw_work)
    console_write("M36-FTRACE: ok symbolize\n");
  else
    console_write("M36-FTRACE: fail symbolize\n");

  console_write("M36-FTRACE: done\n");
}

void m36_diag_run(void) {
  if (!bootinfo_has_flag("b1nix.test=1"))
    return;
  gdb_selftest();
  ftrace_selftest();
}
