/* gdbstub — serial-port GDB remote stub (M36), AArch64.
 *
 * Same protocol engine and same split as kernel/arch/x86_64/gdbstub.c: the
 * packet handler works on a register frame and produces a response payload,
 * framing/checksum happen over a `struct gdb_transport`, and the interactive
 * loop only runs when the kernel was booted with `b1nix.gdb`. What is genuinely
 * different here is the register file: GDB's `org.gnu.gdb.aarch64.core` 'g'
 * packet is x0..x30, sp, pc (all 64-bit) then cpsr (32-bit) — where x86_64 has
 * its 16 GP registers, rip, eflags and the segment selectors. The BRK #imm
 * exception (ESR.EC 0x3C) routes here the way #BP does there.
 */

#include <b1nix/bootinfo.h>
#include <b1nix/arch_aarch64.h>
#include <b1nix/console.h>
#include <b1nix/gdbstub.h>
#include <b1nix/mm.h>
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

/* Register accessors in GDB's aarch64 order: 0..30 = x0..x30, 31 = sp,
 * 32 = pc, 33 = cpsr (32-bit). interrupt_frame is packed, so read/write the
 * members by name rather than indexing through a u64 * into the struct. */
static u64 gdb_reg_get(int idx, int *width, struct interrupt_frame *f) {
  *width = 8;
  switch (idx) {
  case 0: return f->x0;
  case 1: return f->x1;
  case 2: return f->x2;
  case 3: return f->x3;
  case 4: return f->x4;
  case 5: return f->x5;
  case 6: return f->x6;
  case 7: return f->x7;
  case 8: return f->x8;
  case 9: return f->x9;
  case 10: return f->x10;
  case 11: return f->x11;
  case 12: return f->x12;
  case 13: return f->x13;
  case 14: return f->x14;
  case 15: return f->x15;
  case 16: return f->x16;
  case 17: return f->x17;
  case 18: return f->x18;
  case 19: return f->x19;
  case 20: return f->x20;
  case 21: return f->x21;
  case 22: return f->x22;
  case 23: return f->x23;
  case 24: return f->x24;
  case 25: return f->x25;
  case 26: return f->x26;
  case 27: return f->x27;
  case 28: return f->x28;
  case 29: return f->x29;
  case 30: return f->x30;
  case 31: return f->sp_el0;
  case 32: return f->elr;
  default: *width = 4; return f->spsr;
  }
}

static void gdb_reg_set(int idx, u64 val, struct interrupt_frame *f) {
  switch (idx) {
  case 0: f->x0 = val; break;
  case 1: f->x1 = val; break;
  case 2: f->x2 = val; break;
  case 3: f->x3 = val; break;
  case 4: f->x4 = val; break;
  case 5: f->x5 = val; break;
  case 6: f->x6 = val; break;
  case 7: f->x7 = val; break;
  case 8: f->x8 = val; break;
  case 9: f->x9 = val; break;
  case 10: f->x10 = val; break;
  case 11: f->x11 = val; break;
  case 12: f->x12 = val; break;
  case 13: f->x13 = val; break;
  case 14: f->x14 = val; break;
  case 15: f->x15 = val; break;
  case 16: f->x16 = val; break;
  case 17: f->x17 = val; break;
  case 18: f->x18 = val; break;
  case 19: f->x19 = val; break;
  case 20: f->x20 = val; break;
  case 21: f->x21 = val; break;
  case 22: f->x22 = val; break;
  case 23: f->x23 = val; break;
  case 24: f->x24 = val; break;
  case 25: f->x25 = val; break;
  case 26: f->x26 = val; break;
  case 27: f->x27 = val; break;
  case 28: f->x28 = val; break;
  case 29: f->x29 = val; break;
  case 30: f->x30 = val; break;
  case 31: f->sp_el0 = val; break;
  case 32: f->elr = val; break;
  case 33: f->spsr = val; break;
  default: break;
  }
}

#define GDB_REG_LAST 33

usize gdb_build_g_packet(struct interrupt_frame *f, char *out) {
  usize n = 0;
  for (int idx = 0; idx <= GDB_REG_LAST; idx++) {
    int width;
    u64 v = gdb_reg_get(idx, &width, f);
    n += hex_encode_le(v, width, out + n);
  }
  out[n] = '\0';
  return n;
}

/* Memory access guarded so a bad address in an m/M packet can't fault the
 * stub. Two windows are legal on this arch: the boot identity map (RAM from
 * 0x40000000, which is where the kernel image, modules and every pmm frame
 * live) and the kernel virtual area at and above KHEAP_START — heap, large
 * arena, and nothing else below 512 GiB. */
static int gdb_addr_ok(u64 addr) {
  /* Any RAM bank the firmware reported. Written as a constant 0x40000000 here
   * for as long as this port booted only QEMU virt, which rejected every
   * address on a board whose RAM starts at 0 — a Raspberry Pi, where the
   * kernel image itself is at 0x80000. */
  const struct boot_info *bi = bootinfo_get();

  for (usize i = 0; i < bi->memory_region_count; i++) {
    const struct boot_memory_region *r = &bi->memory_regions[i];

    if (r->type != BOOT_MEMORY_AVAILABLE) continue;
    if (addr >= r->base && addr < r->base + r->length)
      return 1;
  }
  if (addr >= KHEAP_START && addr < (512ULL * 1024 * 1024 * 1024))
    return 1;
  /* The kernel image itself, which is mapped whatever the firmware said about
   * RAM. The two need not agree: QEMU's raspi4b describes a single 960 MiB
   * bank at 0 while an image linked for QEMU virt relocates itself to
   * 0x40080000, a gigabyte in - so every address in the running kernel's own
   * text sat outside every reported bank and reading it came back E14. */
  {
    extern char __kernel_start[];
    extern char __kernel_end[];

    if (addr >= (u64)(usize)__kernel_start && addr < (u64)(usize)__kernel_end)
      return 1;
  }
  return 0;
}

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
    for (int idx = 0; idx <= GDB_REG_LAST; idx++) {
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
  f.x0 = 0x1111111111111111ULL;
  f.x1 = 0x2222222222222222ULL;
  f.x2 = 0x3333333333333333ULL;
  f.x3 = 0x4444444444444444ULL;
  f.x29 = 0x7777777777777777ULL;
  f.sp_el0 = 0x8888888888888888ULL;
  /* A real kernel text address, so the m-packet read below is of memory that
   * is genuinely mapped rather than of a hardcoded link address. */
  f.elr = (u64)(usize)&m36_gdb_selftest;
  f.spsr = 0x3c5;

  char out[1200];
  int resume;

  /* Test 1: stop-reply — '?' should return "S05" (SIGTRAP). */
  int len = gdb_handle_packet("?", out, sizeof(out), &f, &resume);
  if (len >= 3 && out[0] == 'S' && out[1] == '0' && out[2] == '5')
    console_write("M36-GDB: ok stop-reply\n");
  else
    console_write("M36-GDB: FAIL stop-reply\n");

  /* Test 2: read-regs — 'g' should return a hex string starting with x0. */
  len = gdb_handle_packet("g", out, sizeof(out), &f, &resume);
  if (len > 16 && strncmp(out, "1111111111111111", 16) == 0)
    console_write("M36-GDB: ok read-regs\n");
  else
    console_write("M36-GDB: FAIL read-regs\n");

  /* Test 3: read-mem — 'm' of a kernel address should return hex bytes. */
  char m_pkt[64];
  usize addr = (usize)f.elr;
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
    __asm__ volatile("yield");
  return (int)(unsigned char)serial_getc();
}
static void serial_putc_t(void *ctx, char c) {
  (void)ctx;
  serial_putc(c);
}

/* Enter the interactive stub on a BRK exception. Only reached when the kernel
 * is booted with b1nix.gdb (see interrupts.c). Loops until a c/s packet tells
 * the target to resume. */
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
