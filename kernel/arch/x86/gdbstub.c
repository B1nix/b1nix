#include <b1nix/arch_x86.h>
#include <b1nix/gdbstub.h>
#include <b1nix/serial.h>
#include <string.h>

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

static usize hex_encode(const u8 *src, usize len, char *out) {
  for (usize i = 0; i < len; i++) {
    out[2 * i] = hex_digit(src[i] >> 4);
    out[2 * i + 1] = hex_digit(src[i] & 0xf);
  }
  return 2 * len;
}

static usize hex_encode_le(u32 val, int bytes, char *out) {
  u8 tmp[4];
  for (int i = 0; i < bytes; i++)
    tmp[i] = (u8)(val >> (8 * i));
  return hex_encode(tmp, (usize)bytes, out);
}

static u32 parse_hex(const char **pp) {
  const char *p = *pp;
  u32 v = 0;
  int d;
  while ((d = hex_val(*p)) >= 0) {
    v = (v << 4) | (u32)d;
    p++;
  }
  *pp = p;
  return v;
}

usize gdb_build_g_packet(struct interrupt_frame *f, char *out) {
  /* i386 GDB register order: eax, ecx, edx, ebx, esp, ebp, esi, edi, eip, eflags, cs, ss, ds, es, fs, gs */
  usize n = 0;
  n += hex_encode_le(f->eax, 4, out + n);
  n += hex_encode_le(f->ecx, 4, out + n);
  n += hex_encode_le(f->edx, 4, out + n);
  n += hex_encode_le(f->ebx, 4, out + n);
  n += hex_encode_le(f->esp, 4, out + n);
  n += hex_encode_le(f->ebp, 4, out + n);
  n += hex_encode_le(f->esi, 4, out + n);
  n += hex_encode_le(f->edi, 4, out + n);
  n += hex_encode_le(f->eip, 4, out + n);
  n += hex_encode_le(f->eflags, 4, out + n);
  n += hex_encode_le(f->cs, 4, out + n);
  n += hex_encode_le(f->ss, 4, out + n);
  n += hex_encode_le(0x23, 4, out + n); /* ds */
  n += hex_encode_le(0x23, 4, out + n); /* es */
  n += hex_encode_le(0x23, 4, out + n); /* fs */
  n += hex_encode_le(0x33, 4, out + n); /* gs */
  out[n] = '\0';
  return n;
}

static u32 gdb_reg_get(int idx, int *width, struct interrupt_frame *f) {
  *width = 4;
  switch (idx) {
  case 0: return f->eax;
  case 1: return f->ecx;
  case 2: return f->edx;
  case 3: return f->ebx;
  case 4: return f->esp;
  case 5: return f->ebp;
  case 6: return f->esi;
  case 7: return f->edi;
  case 8: return f->eip;
  case 9: return f->eflags;
  case 10: return f->cs;
  case 11: return f->ss;
  default: return 0;
  }
}

static void gdb_reg_set(int idx, u32 val, struct interrupt_frame *f) {
  switch (idx) {
  case 0: f->eax = val; break;
  case 1: f->ecx = val; break;
  case 2: f->edx = val; break;
  case 3: f->ebx = val; break;
  case 4: f->esp = val; break;
  case 5: f->ebp = val; break;
  case 6: f->esi = val; break;
  case 7: f->edi = val; break;
  case 8: f->eip = val; break;
  case 9: f->eflags = val; break;
  case 10: f->cs = val; break;
  case 11: f->ss = val; break;
  default: break;
  }
}

static int gdb_addr_ok(u32 addr) {
  if (addr >= 0x1000 && addr < 0x80000000)
    return 1;
  if (addr >= 0x80000000 && addr < 0xffffffff)
    return 1;
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
    strcpy(out, "S05");
    break;

  case 'g':
    gdb_build_g_packet(f, out);
    break;

  case 'G': {
    const char *p = in + 1;
    for (int idx = 0; idx <= 11; idx++) {
      int width;
      (void)gdb_reg_get(idx, &width, f);
      u32 v = 0;
      for (int b = 0; b < width; b++) {
        int hi = hex_val(*p++), lo = hex_val(*p++);
        if (hi < 0 || lo < 0)
          goto g_done;
        v |= (u32)((hi << 4) | lo) << (8 * b);
      }
      gdb_reg_set(idx, v, f);
    }
  g_done:
    strcpy(out, "OK");
    break;
  }

  case 'm': {
    const char *p = in + 1;
    u32 addr = parse_hex(&p);
    if (*p == ',')
      p++;
    u32 len = parse_hex(&p);
    if (len * 2 + 1 > outcap)
      len = (u32)(outcap - 1) / 2;
    usize n = 0;
    for (u32 i = 0; i < len; i++) {
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
    const char *p = in + 1;
    u32 addr = parse_hex(&p);
    if (*p == ',')
      p++;
    u32 len = parse_hex(&p);
    if (*p == ':')
      p++;
    for (u32 i = 0; i < len; i++) {
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
    break;

  default:
    break;
  }
  return (int)strlen(out);
}

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

void gdb_stub_enter(struct interrupt_frame *f) {
  struct gdb_transport t = {serial_getc_blocking, serial_putc_t, 0};
  char pkt[1200];
  char resp[1200];
  gdb_send_packet(&t, "S05");
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
