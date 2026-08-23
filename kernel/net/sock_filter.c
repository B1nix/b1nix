/* Classic BPF socket filters (SO_ATTACH_FILTER).
 *
 * The engine is deliberately the plain interpreter Linux started with: a
 * verified instruction array, an accumulator, an index register and sixteen
 * scratch words. Filters attached to a socket are small — systemd's device
 * monitor installs about a dozen instructions — and an interpreter that is
 * obviously correct is worth more here than a JIT.
 *
 * Packet loads are big-endian by definition of the instruction set (BPF_ABS
 * and BPF_IND load network byte order), which is why a filter written against
 * a host-endian structure compares against htonl() of the value it wants.
 * Getting that backwards would silently drop every message instead of
 * failing, so it is the one thing in here worth reading twice.
 */

#include <b1nix/mm.h>
#include <b1nix/sock_filter.h>
#include <string.h>

/* Instruction encoding (Linux's <linux/filter.h> values, which are ABI). */
#define BPF_CLASS(code) ((code) & 0x07)
#define BPF_LD 0x00
#define BPF_LDX 0x01
#define BPF_ST 0x02
#define BPF_STX 0x03
#define BPF_ALU 0x04
#define BPF_JMP 0x05
#define BPF_RET 0x06
#define BPF_MISC 0x07

#define BPF_SIZE(code) ((code) & 0x18)
#define BPF_W 0x00
#define BPF_H 0x08
#define BPF_B 0x10

#define BPF_MODE(code) ((code) & 0xe0)
#define BPF_IMM 0x00
#define BPF_ABS 0x20
#define BPF_IND 0x40
#define BPF_MEM 0x60
#define BPF_LEN 0x80
#define BPF_MSH 0xa0

#define BPF_OP(code) ((code) & 0xf0)
#define BPF_ADD 0x00
#define BPF_SUB 0x10
#define BPF_MUL 0x20
#define BPF_DIV 0x30
#define BPF_OR 0x40
#define BPF_AND 0x50
#define BPF_LSH 0x60
#define BPF_RSH 0x70
#define BPF_NEG 0x80
#define BPF_MOD 0x90
#define BPF_XOR 0xa0

#define BPF_JA 0x00
#define BPF_JEQ 0x10
#define BPF_JGT 0x20
#define BPF_JGE 0x30
#define BPF_JSET 0x40

#define BPF_SRC(code) ((code) & 0x08)
#define BPF_K 0x00
#define BPF_X 0x08

#define BPF_RVAL(code) ((code) & 0x18)
#define BPF_A 0x10

#define BPF_MISCOP(code) ((code) & 0xf8)
#define BPF_TAX 0x00
#define BPF_TXA 0x80

#define BPF_MEMWORDS 16

/* ── Verifier ─────────────────────────────────────────────────────────────
 *
 * Linux refuses a program it cannot prove terminates and stays inside itself.
 * The same three rules are enough: every jump lands inside the program and
 * forward, the last instruction is a return, and no opcode is one we cannot
 * execute. Anything rejected here would otherwise be a wild read inside an
 * interrupt-time delivery path.
 */
static int sock_filter_check(const struct sock_filter_insn *f, u32 len) {
  if (len == 0 || len > BPF_MAXINSNS)
    return 0;

  for (u32 pc = 0; pc < len; pc++) {
    u16 code = f[pc].code;
    switch (BPF_CLASS(code)) {
    case BPF_LD:
    case BPF_LDX:
      switch (BPF_MODE(code)) {
      case BPF_IMM:
      case BPF_ABS:
      case BPF_IND:
      case BPF_LEN:
        break;
      case BPF_MSH:
        /* Only LDX B MSH exists. */
        if (BPF_CLASS(code) != BPF_LDX || BPF_SIZE(code) != BPF_B)
          return 0;
        break;
      case BPF_MEM:
        if (f[pc].k >= BPF_MEMWORDS)
          return 0;
        break;
      default:
        return 0;
      }
      break;
    case BPF_ST:
    case BPF_STX:
      if (f[pc].k >= BPF_MEMWORDS)
        return 0;
      break;
    case BPF_ALU:
      switch (BPF_OP(code)) {
      case BPF_ADD:
      case BPF_SUB:
      case BPF_MUL:
      case BPF_OR:
      case BPF_AND:
      case BPF_LSH:
      case BPF_RSH:
      case BPF_NEG:
      case BPF_XOR:
        break;
      case BPF_DIV:
      case BPF_MOD:
        /* A divide by an immediate zero can only ever fault. */
        if (BPF_SRC(code) == BPF_K && f[pc].k == 0)
          return 0;
        break;
      default:
        return 0;
      }
      break;
    case BPF_JMP: {
      u32 rest = len - pc - 1;
      switch (BPF_OP(code)) {
      case BPF_JA:
        if (f[pc].k >= rest)
          return 0;
        break;
      case BPF_JEQ:
      case BPF_JGT:
      case BPF_JGE:
      case BPF_JSET:
        if (f[pc].jt >= rest || f[pc].jf >= rest)
          return 0;
        break;
      default:
        return 0;
      }
      break;
    }
    case BPF_RET:
      if (BPF_RVAL(code) != BPF_K && BPF_RVAL(code) != BPF_A)
        return 0;
      break;
    case BPF_MISC:
      if (BPF_MISCOP(code) != BPF_TAX && BPF_MISCOP(code) != BPF_TXA)
        return 0;
      break;
    default:
      return 0;
    }
  }

  /* Falling off the end has no defined result, so it is not allowed. */
  return BPF_CLASS(f[len - 1].code) == BPF_RET;
}

struct sock_filter_prog *sock_filter_compile(const struct sock_filter_insn *insns,
                                             u32 len) {
  if (!insns || !sock_filter_check(insns, len))
    return 0;
  struct sock_filter_prog *p = (struct sock_filter_prog *)kmalloc(
      sizeof(*p) + (usize)len * sizeof(insns[0]));
  if (!p)
    return 0;
  p->len = len;
  memcpy(p->insns, insns, (usize)len * sizeof(insns[0]));
  return p;
}

/* ── Interpreter ──────────────────────────────────────────────────────────
 *
 * Packet loads are bounds-checked against the datagram and a load that runs
 * off the end drops the packet, which is exactly what Linux does: a filter
 * reading past the data it was given has already been told something it does
 * not know, and guessing zero for the rest would let it accept on a lie.
 */
static int bpf_load(const u8 *data, u32 len, u32 off, int size, u32 *out) {
  if (size == BPF_W) {
    if (off + 4 > len || off + 4 < off)
      return 0;
    *out = ((u32)data[off] << 24) | ((u32)data[off + 1] << 16) |
           ((u32)data[off + 2] << 8) | (u32)data[off + 3];
    return 1;
  }
  if (size == BPF_H) {
    if (off + 2 > len || off + 2 < off)
      return 0;
    *out = ((u32)data[off] << 8) | (u32)data[off + 1];
    return 1;
  }
  if (off >= len)
    return 0;
  *out = data[off];
  return 1;
}

u32 sock_filter_run(const struct sock_filter_prog *prog, const u8 *data,
                    u32 len) {
  if (!prog)
    return (u32)-1;
  if (!data)
    len = 0;

  u32 A = 0, X = 0;
  u32 mem[BPF_MEMWORDS];
  memset(mem, 0, sizeof(mem));

  const struct sock_filter_insn *f = prog->insns;
  for (u32 pc = 0; pc < prog->len; pc++) {
    u16 code = f[pc].code;
    u32 k = f[pc].k;
    u32 v;

    switch (BPF_CLASS(code)) {
    case BPF_LD:
      switch (BPF_MODE(code)) {
      case BPF_IMM: A = k; break;
      case BPF_LEN: A = len; break;
      case BPF_MEM: A = mem[k]; break;
      case BPF_ABS:
        if (!bpf_load(data, len, k, BPF_SIZE(code), &v))
          return 0;
        A = v;
        break;
      case BPF_IND:
        if (!bpf_load(data, len, X + k, BPF_SIZE(code), &v))
          return 0;
        A = v;
        break;
      default: return 0;
      }
      break;

    case BPF_LDX:
      switch (BPF_MODE(code)) {
      case BPF_IMM: X = k; break;
      case BPF_LEN: X = len; break;
      case BPF_MEM: X = mem[k]; break;
      case BPF_ABS:
        if (!bpf_load(data, len, k, BPF_SIZE(code), &v))
          return 0;
        X = v;
        break;
      case BPF_IND:
        if (!bpf_load(data, len, X + k, BPF_SIZE(code), &v))
          return 0;
        X = v;
        break;
      case BPF_MSH:
        /* The IPv4 header-length idiom: X = 4 * (data[k] & 0xf). */
        if (!bpf_load(data, len, k, BPF_B, &v))
          return 0;
        X = (v & 0x0f) << 2;
        break;
      default: return 0;
      }
      break;

    case BPF_ST: mem[k] = A; break;
    case BPF_STX: mem[k] = X; break;

    case BPF_ALU: {
      u32 src = (BPF_SRC(code) == BPF_X) ? X : k;
      switch (BPF_OP(code)) {
      case BPF_ADD: A += src; break;
      case BPF_SUB: A -= src; break;
      case BPF_MUL: A *= src; break;
      case BPF_DIV:
        if (src == 0)
          return 0;
        A /= src;
        break;
      case BPF_MOD:
        if (src == 0)
          return 0;
        A %= src;
        break;
      case BPF_OR: A |= src; break;
      case BPF_AND: A &= src; break;
      /* A shift by 32 or more is undefined in C; Linux's interpreter
       * produces zero for it, so say so explicitly. */
      case BPF_LSH: A = (src >= 32) ? 0 : (A << src); break;
      case BPF_RSH: A = (src >= 32) ? 0 : (A >> src); break;
      case BPF_XOR: A ^= src; break;
      case BPF_NEG: A = (u32)(-(i32)A); break;
      default: return 0;
      }
      break;
    }

    case BPF_JMP: {
      if (BPF_OP(code) == BPF_JA) {
        pc += k;
        break;
      }
      u32 src = (BPF_SRC(code) == BPF_X) ? X : k;
      int taken;
      switch (BPF_OP(code)) {
      case BPF_JEQ: taken = (A == src); break;
      case BPF_JGT: taken = (A > src); break;
      case BPF_JGE: taken = (A >= src); break;
      case BPF_JSET: taken = ((A & src) != 0); break;
      default: return 0;
      }
      pc += taken ? f[pc].jt : f[pc].jf;
      break;
    }

    case BPF_RET:
      return (BPF_RVAL(code) == BPF_A) ? A : k;

    case BPF_MISC:
      if (BPF_MISCOP(code) == BPF_TAX)
        X = A;
      else
        A = X;
      break;

    default:
      return 0;
    }
  }

  /* Unreachable: the verifier requires the last instruction to be a return. */
  return 0;
}
