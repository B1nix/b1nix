/* gdbstub — serial-port GDB remote stub (M36).
 *
 * Implements the GDB Remote Serial Protocol (RSP) over COM1 so a host
 * `gdb` / `lldb` can attach with `target remote`. The protocol engine is split
 * from the transport so it can be unit-tested in-kernel without a live host:
 *
 *   - gdb_handle_packet() processes ONE decoded packet against a register
 *     frame and produces the response payload (no $..#cc framing);
 *   - gdb_recv_packet()/gdb_send_packet() do the framing + checksum + ack over
 *     a `struct gdb_transport` (serial in production, an in-memory buffer in
 *     the self-test).
 *
 * The interactive serial loop (gdb_stub_enter) only runs when the kernel is
 * booted with `b1nix.gdb`, so it never stalls an ordinary/test boot. The
 * int3 (#BP) exception routes here when the stub is active.
 *
 * Supported packets: ? (stop reason), g/G (read/write registers),
 * m/M (read/write memory), c (continue), s (step), and the standard
 * qSupported handshake. Unknown packets get the empty "" response per spec.
 */

#include <b1nix/arch_x86_64.h>
#include <b1nix/console.h>
#include <b1nix/gdbstub.h>
#include <b1nix/serial.h>
#include <string.h>

/* ── hex helpers ── */
static char hex_digit(int v) {
  v &= 0xf;
  return (char)(v < 10 ? '0' + v : 'a' + v - 10);
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Encode `len` bytes as 2*len lowercase hex chars. Returns chars written. */
static usize hex_encode(const u8 *src, usize len, char *out) {
  for (usize i = 0; i < len; i++) {
    out[2 * i] = hex_digit(src[i] >> 4);
    out[2 * i + 1] = hex_digit(src[i] & 0xf);
  }
  return 2 * len;
}

/* Encode a value little-endian as `bytes`*2 hex chars (GDB register order). */
static usize hex_encode_le(u64 val, int bytes, char *out) {
  u8 tmp[8];
  for (int i = 0; i < bytes; i++)
    tmp[i] = (u8)(val >> (8 * i));
  return hex_encode(tmp, (usize)bytes, out);
}

/* Parse a hex integer up to a delimiter; advances *pp past the digits. */
static u64 parse_hex(const char **pp) {
  const char *p = *pp;
  u64 v = 0;
  int d;
  while ((d = hex_val(*p)) >= 0) {
    v = (v << 4) | (u64)d;
    p++;
  }
  *pp = p;
  return v;
}

/* GDB x86_64 'g' packet register order. 16 GP regs + rip are 64-bit; eflags
 * and the segment selectors are 32-bit. */
usize gdb_build_g_packet(struct interrupt_frame *f, char *out) {
  usize n = 0;
  n += hex_encode_le(f->rax, 8, out + n);
  n += hex_encode_le(f->rbx, 8, out + n);
  n += hex_encode_le(f->rcx, 8, out + n);
  n += hex_encode_le(f->rdx, 8, out + n);
  n += hex_encode_le(f->rsi, 8, out + n);
  n += hex_encode_le(f->rdi, 8, out + n);
  n += hex_encode_le(f->rbp, 8, out + n);
  n += hex_encode_le(f->rsp, 8, out + n);
  n += hex_encode_le(f->r8, 8, out + n);
  n += hex_encode_le(f->r9, 8, out + n);
  n += hex_encode_le(f->r10, 8, out + n);
  n += hex_encode_le(f->r11, 8, out + n);
  n += hex_encode_le(f->r12, 8, out + n);
  n += hex_encode_le(f->r13, 8, out + n);
  n += hex_encode_le(f->r14, 8, out + n);
  n += hex_encode_le(f->r15, 8, out + n);
  n += hex_encode_le(f->rip, 8, out + n);
  n += hex_encode_le(f->rflags, 4, out + n);
  n += hex_encode_le(f->cs, 4, out + n);
  n += hex_encode_le(f->ss, 4, out + n);
  n += hex_encode_le(0, 4, out + n); /* ds */
  n += hex_encode_le(0, 4, out + n); /* es */
  n += hex_encode_le(0, 4, out + n); /* fs */
  n += hex_encode_le(0, 4, out + n); /* gs */
  out[n] = '\0';
  return n;
}

/* Register accessors mirroring gdb_build_g_packet. interrupt_frame is packed,
 * so avoid taking member addresses: clang quite rightly warns that a u64 *
 * could be unaligned even though x86 would tolerate it. */
static u64 gdb_reg_get(int idx, int *width, struct interrupt_frame *f) {
  *width = 8;
  switch (idx) {
  case 0: return f->rax;
  case 1: return f->rbx;
  case 2: return f->rcx;
  case 3: return f->rdx;
  case 4: return f->rsi;
  case 5: return f->rdi;
  case 6: return f->rbp;
  case 7: return f->rsp;
  case 8: return f->r8;
  case 9: return f->r9;
  case 10: return f->r10;
  case 11: return f->r11;
  case 12: return f->r12;
  case 13: return f->r13;
  case 14: return f->r14;
  case 15: return f->r15;
  case 16: return f->rip;
  case 17: *width = 4; return f->rflags;
  default: *width = 4; return 0;
  }
}

static void gdb_reg_set(int idx, u64 val, struct interrupt_frame *f) {
  switch (idx) {
  case 0: f->rax = val; break;
  case 1: f->rbx = val; break;
  case 2: f->rcx = val; break;
  case 3: f->rdx = val; break;
  case 4: f->rsi = val; break;
  case 5: f->rdi = val; break;
  case 6: f->rbp = val; break;
  case 7: f->rsp = val; break;
  case 8: f->r8 = val; break;
  case 9: f->r9 = val; break;
  case 10: f->r10 = val; break;
  case 11: f->r11 = val; break;
  case 12: f->r12 = val; break;
  case 13: f->r13 = val; break;
  case 14: f->r14 = val; break;
  case 15: f->r15 = val; break;
  case 16: f->rip = val; break;
  case 17: f->rflags = val; break;
  default: break;
  }
}

/* Memory access guarded so a bad address in an m/M packet can't fault the
 * stub. Mirrors the kernel-text range check used by the backtrace. */
static int gdb_addr_ok(u64 addr) {
  if (addr >= 0x1000ULL && addr < 0x0000000100000000ULL)
    return 1;
  if (addr >= 0xffff800000000000ULL && addr < 0xffff800100000000ULL)
    return 1;
  /* Higher-half kernel window (0xFFFFFFFF80000000 + 1 GiB): kernel code/data
   * symbols now live here, so g/G register dumps and m/M of a kernel address
   * land in this range. */
  if (addr >= 0xffffffff80000000ULL && addr < 0xffffffffc0000000ULL)
    return 1;
  return 0;
}

/* Handle one decoded packet. `in` is the payload (no $, #, checksum). Writes
 * the response payload (NUL-terminated) into `out` (outcap bytes). Returns the
 * response length. *resume is set non-zero for c/s (caller should leave the
 * stub loop and continue execution). */
int gdb_handle_packet(const char *in, char *out, usize outcap,
                      struct interrupt_frame *f, int *resume) {
  if (resume)
    *resume = 0;
  if (outcap == 0)
    return 0;
  out[0] = '\0';

  switch (in[0]) {
  case '?':
    /* Last stop reason: SIGTRAP. */
    strcpy(out, "S05");
    break;

  case 'g':
    gdb_build_g_packet(f, out);
    break;

  case 'G': {
    /* Write all registers from hex. */
    const char *p = in + 1;
    for (int idx = 0; idx <= 17; idx++) {
      int width;
      (void)gdb_reg_get(idx, &width, f);
      u64 v = 0;
      for (int b = 0; b < width; b++) {
        int hi = hex_val(*p++), lo = hex_val(*p++);
        if (hi < 0 || lo < 0)
          goto g_done;
        v |= (u64)((hi << 4) | lo) << (8 * b);
      }
      gdb_reg_set(idx, v, f);
    }
  g_done:
    strcpy(out, "OK");
    break;
  }

  case 'm': {
    /* m addr,len → hex bytes. */
    const char *p = in + 1;
    u64 addr = parse_hex(&p);
    if (*p == ',')
      p++;
    u64 len = parse_hex(&p);
    if (len * 2 + 1 > outcap)
      len = (outcap - 1) / 2;
    usize n = 0;
    for (u64 i = 0; i < len; i++) {
      if (!gdb_addr_ok(addr + i)) {
        strcpy(out, "E14");
        return 3;
      }
      u8 byte = *(volatile u8 *)(usize)(addr + i);
      n += hex_encode(&byte, 1, out + n);
    }
    out[n] = '\0';
    return (int)n;
  }

  case 'M': {
    /* M addr,len:data */
    const char *p = in + 1;
    u64 addr = parse_hex(&p);
    if (*p == ',')
      p++;
    u64 len = parse_hex(&p);
    if (*p == ':')
      p++;
    for (u64 i = 0; i < len; i++) {
      if (!gdb_addr_ok(addr + i)) {
        strcpy(out, "E14");
        return 3;
      }
      int hi = hex_val(*p++), lo = hex_val(*p++);
      if (hi < 0 || lo < 0)
        break;
      *(volatile u8 *)(usize)(addr + i) = (u8)((hi << 4) | lo);
    }
    strcpy(out, "OK");
    break;
  }

  case 'c':
  case 's':
    if (resume)
      *resume = 1;
    /* No immediate response — the target resumes; a stop reply follows when it
     * halts again. For the stub's single-shot use we ack with S05. */
    strcpy(out, "S05");
    break;

  case 'q':
    if (strncmp(in, "qSupported", 10) == 0)
      strcpy(out, "PacketSize=1000");
    else if (strcmp(in, "qC") == 0)
      strcpy(out, "QC01");
    else if (strcmp(in, "qAttached") == 0)
      strcpy(out, "1");
    /* else: empty (unsupported query). */
    break;

  default:
    /* Unsupported → empty response per RSP. */
    break;
  }
  return (int)strlen(out);
}

/* ── framing over a transport ── */
static u8 gdb_checksum(const char *s, usize len) {
  u8 sum = 0;
  for (usize i = 0; i < len; i++)
    sum += (u8)s[i];
  return sum;
}

void gdb_send_packet(struct gdb_transport *t, const char *payload) {
  usize len = strlen(payload);
  u8 sum = gdb_checksum(payload, len);
  t->putc(t->ctx, '$');
  for (usize i = 0; i < len; i++)
    t->putc(t->ctx, payload[i]);
  t->putc(t->ctx, '#');
  t->putc(t->ctx, hex_digit(sum >> 4));
  t->putc(t->ctx, hex_digit(sum & 0xf));
}

/* Receive one packet payload into buf (capacity cap). Returns payload length,
 * or -1 on transport EOF. Sends '+' ack on good checksum, '-' to request a
 * resend on mismatch. */
int gdb_recv_packet(struct gdb_transport *t, char *buf, usize cap) {
  int c;
  /* Skip to packet start '$'. */
  do {
    c = t->getc(t->ctx);
    if (c < 0)
      return -1;
  } while (c != '$');

  usize n = 0;
  u8 sum = 0;
  for (;;) {
    c = t->getc(t->ctx);
    if (c < 0)
      return -1;
    if (c == '#')
      break;
    if (n + 1 < cap)
      buf[n++] = (char)c;
    sum += (u8)c;
  }
  buf[n] = '\0';
  int h = t->getc(t->ctx), l = t->getc(t->ctx);
  if (h < 0 || l < 0)
    return -1;
  u8 want = (u8)((hex_val((char)h) << 4) | hex_val((char)l));
  if (want == sum) {
    t->putc(t->ctx, '+');
    return (int)n;
  }
  t->putc(t->ctx, '-');
  return (int)n;
}

/* ── in-memory self-test (M36 smoke, called from main.c in test mode) ── */

struct mem_transport {
  char buf[2048];
  usize pos;
  usize read_pos;
};

static int mem_getc(void *ctx) {
  struct mem_transport *m = ctx;
  if (m->read_pos >= m->pos) return -1;
  return (unsigned char)m->buf[m->read_pos++];
}

static void mem_putc(void *ctx, char c) {
  struct mem_transport *m = ctx;
  if (m->pos < sizeof(m->buf))
    m->buf[m->pos++] = c;
}

static void mem_reset(struct mem_transport *m) {
  m->pos = 0;
  m->read_pos = 0;
}

void m36_gdb_selftest(void) {
  console_write("M36-GDB: start\n");

  struct interrupt_frame f;
  memset(&f, 0, sizeof(f));
  f.rax = 0x1111111111111111ULL;
  f.rbx = 0x2222222222222222ULL;
  f.rcx = 0x3333333333333333ULL;
  f.rdx = 0x4444444444444444ULL;
  f.rsi = 0x5555555555555555ULL;
  f.rdi = 0x6666666666666666ULL;
  f.rbp = 0x7777777777777777ULL;
  f.rsp = 0x8888888888888888ULL;
  f.rip = 0xFFFFFFFF80100000ULL;
  f.rflags = 0x202;

  char out[1200];
  int resume;

  /* Test 1: stop-reply — '?' should return "S05" (SIGTRAP). */
  int len = gdb_handle_packet("?", out, sizeof(out), &f, &resume);
  if (len >= 3 && out[0] == 'S' && out[1] == '0' && out[2] == '5')
    console_write("M36-GDB: ok stop-reply\n");
  else
    console_write("M36-GDB: FAIL stop-reply\n");

  /* Test 2: read-regs — 'g' should return a hex string starting with rax. */
  len = gdb_handle_packet("g", out, sizeof(out), &f, &resume);
  if (len > 16 && strncmp(out, "1111111111111111", 16) == 0)
    console_write("M36-GDB: ok read-regs\n");
  else
    console_write("M36-GDB: FAIL read-regs\n");

  /* Test 3: read-mem — 'm' of a kernel address should return hex bytes. */
  char m_pkt[64];
  usize addr = (usize)f.rip;
  int plen = 0;
  m_pkt[plen++] = 'm';
  for (int i = 60; i >= 0; i -= 4)
    m_pkt[plen++] = hex_digit((int)((addr >> i) & 0xf));
  m_pkt[plen++] = ',';
  m_pkt[plen++] = '1';
  m_pkt[plen++] = '0';
  m_pkt[plen] = '\0';

  len = gdb_handle_packet(m_pkt, out, sizeof(out), &f, &resume);
  if (len == 32 && out[0] != 'E')
    console_write("M36-GDB: ok read-mem\n");
  else
    console_write("M36-GDB: FAIL read-mem\n");

  /* Test 4: framing — send_packet + recv_packet round-trip over in-memory. */
  struct mem_transport mt;
  mem_reset(&mt);
  struct gdb_transport t = {mem_getc, mem_putc, &mt};
  gdb_send_packet(&t, "hello");
  char pkt_buf[128];
  int rlen = gdb_recv_packet(&t, pkt_buf, sizeof(pkt_buf));
  if (rlen == 5 && strcmp(pkt_buf, "hello") == 0)
    console_write("M36-GDB: ok framing\n");
  else
    console_write("M36-GDB: FAIL framing\n");

  console_write("M36-GDB: done\n");
}

/* ── serial transport (production) ── */
static int serial_getc_blocking(void *ctx) {
  (void)ctx;
  while (!serial_has_data())
    __asm__ volatile("pause");
  return (int)(unsigned char)serial_getc();
}
static void serial_putc_t(void *ctx, char c) {
  (void)ctx;
  serial_putc(c);
}

/* Enter the interactive stub on an exception/breakpoint. Only reached when the
 * kernel is booted with b1nix.gdb (see interrupts.c). Loops until a c/s packet
 * tells the target to resume. */
void gdb_stub_enter(struct interrupt_frame *f) {
  struct gdb_transport t = {serial_getc_blocking, serial_putc_t, 0};
  char pkt[1200];
  char resp[1200];
  gdb_send_packet(&t, "S05"); /* announce the stop */
  for (;;) {
    int len = gdb_recv_packet(&t, pkt, sizeof(pkt));
    if (len < 0)
      return;
    if (len == 0)
      continue;
    int resume = 0;
    gdb_handle_packet(pkt, resp, sizeof(resp), f, &resume);
    gdb_send_packet(&t, resp);
    if (resume)
      return;
  }
}
