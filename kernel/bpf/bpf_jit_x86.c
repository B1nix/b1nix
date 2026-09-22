/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The eBPF JIT for x86_64.
 *
 * WHY
 *
 * A tracing program runs at the site it was attached to -- on a system call,
 * on a scheduler switch, on every sample a perf event takes -- and the
 * interpreter costs a switch on the opcode, a bounds check and a memory
 * indirection per instruction. Translated once at load time, the same program
 * is a straight line of machine code: the register file lives in registers, a
 * branch is a branch, and a helper call is a call. That is the difference
 * between a probe that can be left on and one that distorts what it measures.
 *
 * WHAT IT TRANSLATES
 *
 * Everything the verifier lets through except division, remainder and the byte
 * swaps: a program containing one of those is not compiled and runs on the
 * interpreter instead, so nothing is refused for want of a JIT. The same
 * translation table can grow later; a missing opcode costs speed, never
 * correctness.
 *
 * THE REGISTER MAP is Linux's, so the calling convention falls out for free:
 *
 *   r0  rax   r1  rdi   r2  rsi   r3  rdx   r4  rcx   r5  r8
 *   r6  rbx   r7  r13   r8  r14   r9  r15   r10 rbp (the frame base)
 *
 * r1 is the first argument register, so the context the program is called with
 * arrives where BPF expects it; r6-r9 are callee-saved on x86, so they survive
 * a helper call exactly as the BPF ABI promises; r10 is rbp, and the 512-byte
 * frame is what the prologue reserves below it.
 *
 * SAFETY is the verifier's, not this file's. The code emitted here performs
 * the loads and stores the verifier already proved in bounds, with no checks of
 * its own -- the same bargain Linux's JIT makes. The one thing this file owes
 * the kernel is that it emits what the instruction means, which is why the
 * unsupported opcodes bail out rather than approximate.
 */

#include <b1nix/bpf.h>
#include <b1nix/bpf_insn.h>

#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/module.h>

#include <string.h>

#if defined(__x86_64__)

/* x86 register numbers, indexed by BPF register. */
static const u8 jit_reg[11] = {
    0,  /* r0  -> rax */
    7,  /* r1  -> rdi */
    6,  /* r2  -> rsi */
    2,  /* r3  -> rdx */
    1,  /* r4  -> rcx */
    8,  /* r5  -> r8  */
    3,  /* r6  -> rbx */
    13, /* r7  -> r13 */
    14, /* r8  -> r14 */
    15, /* r9  -> r15 */
    5,  /* r10 -> rbp */
};

#define X86_RAX 0
#define X86_RCX 1

/* Condition codes, for the 0x0F 0x8x jumps. */
#define CC_E 0x4
#define CC_NE 0x5
#define CC_B 0x2
#define CC_AE 0x3
#define CC_BE 0x6
#define CC_A 0x7
#define CC_L 0xC
#define CC_GE 0xD
#define CC_LE 0xE
#define CC_G 0xF

struct jit {
  u8 *buf;
  usize len;
  usize cap;
  int fail; /* an opcode this does not translate, or no room */
};

static void e8(struct jit *j, u8 b) {
  if (j->len + 1 > j->cap) {
    j->fail = 1;
    return;
  }
  j->buf[j->len++] = b;
}

static void e32(struct jit *j, u32 v) {
  for (int i = 0; i < 4; i++)
    e8(j, (u8)(v >> (8 * i)));
}

static void e64(struct jit *j, u64 v) {
  for (int i = 0; i < 8; i++)
    e8(j, (u8)(v >> (8 * i)));
}

/* The REX prefix. `force` emits it even when every bit is zero, which an 8-bit
 * register operand needs to name sil/dil/spl/bpl rather than the high halves of
 * ax/cx/dx/bx. */
static void rex(struct jit *j, int w, int r, int x, int b, int force) {
  u8 v = (u8)(0x40 | (w << 3) | (r << 2) | (x << 1) | b);

  if (v != 0x40 || force)
    e8(j, v);
}

/* op r/m64, r64 — the form whose modrm has the source in `reg`. */
static void alu_rr(struct jit *j, u8 op, int w, u8 dst, u8 src) {
  rex(j, w, src >= 8, 0, dst >= 8, 0);
  e8(j, op);
  e8(j, (u8)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

/* The 0x81 group: add=0 or=1 and=4 sub=5 xor=6 cmp=7. */
static void alu_ri(struct jit *j, u8 digit, int w, u8 dst, u32 imm) {
  rex(j, w, 0, 0, dst >= 8, 0);
  e8(j, 0x81);
  e8(j, (u8)(0xC0 | (digit << 3) | (dst & 7)));
  e32(j, imm);
}

static void mov_ri32(struct jit *j, int w, u8 dst, u32 imm) {
  rex(j, w, 0, 0, dst >= 8, 0);
  e8(j, 0xC7);
  e8(j, (u8)(0xC0 | (dst & 7)));
  e32(j, imm);
}

static void mov_ri64(struct jit *j, u8 dst, u64 imm) {
  rex(j, 1, 0, 0, dst >= 8, 0);
  e8(j, (u8)(0xB8 | (dst & 7)));
  e64(j, imm);
}

static void imul_rr(struct jit *j, int w, u8 dst, u8 src) {
  rex(j, w, dst >= 8, 0, src >= 8, 0);
  e8(j, 0x0F);
  e8(j, 0xAF);
  e8(j, (u8)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

static void imul_ri(struct jit *j, int w, u8 dst, u32 imm) {
  rex(j, w, dst >= 8, 0, dst >= 8, 0);
  e8(j, 0x69);
  e8(j, (u8)(0xC0 | ((dst & 7) << 3) | (dst & 7)));
  e32(j, imm);
}

/* The 0xF7 group: not=2 neg=3, and test with an immediate is digit 0. */
static void grp3(struct jit *j, u8 digit, int w, u8 dst) {
  rex(j, w, 0, 0, dst >= 8, 0);
  e8(j, 0xF7);
  e8(j, (u8)(0xC0 | (digit << 3) | (dst & 7)));
}

/* Shifts: digit 4 = shl, 5 = shr, 7 = sar. */
static void shift_ri(struct jit *j, u8 digit, int w, u8 dst, u8 amount) {
  rex(j, w, 0, 0, dst >= 8, 0);
  e8(j, 0xC1);
  e8(j, (u8)(0xC0 | (digit << 3) | (dst & 7)));
  e8(j, amount);
}

static void shift_cl(struct jit *j, u8 digit, int w, u8 dst) {
  rex(j, w, 0, 0, dst >= 8, 0);
  e8(j, 0xD3);
  e8(j, (u8)(0xC0 | (digit << 3) | (dst & 7)));
}

/* modrm + disp32 for [base + off]. */
static void mem_operand(struct jit *j, u8 reg, u8 base, i32 off) {
  e8(j, (u8)(0x80 | ((reg & 7) << 3) | (base & 7)));
  if ((base & 7) == 4)
    e8(j, 0x24); /* rsp/r12 need a SIB byte; neither is in the map, but be safe */
  e32(j, (u32)off);
}

/* dst = *(size *)(base + off), zero-extended. */
static void emit_load(struct jit *j, int size, u8 dst, u8 base, i32 off) {
  switch (size) {
  case 8:
    rex(j, 1, dst >= 8, 0, base >= 8, 0);
    e8(j, 0x8B);
    break;
  case 4:
    rex(j, 0, dst >= 8, 0, base >= 8, 0);
    e8(j, 0x8B);
    break;
  case 2:
    rex(j, 0, dst >= 8, 0, base >= 8, 0);
    e8(j, 0x0F);
    e8(j, 0xB7);
    break;
  default:
    rex(j, 0, dst >= 8, 0, base >= 8, 0);
    e8(j, 0x0F);
    e8(j, 0xB6);
    break;
  }
  mem_operand(j, dst, base, off);
}

/* *(size *)(base + off) = src. */
static void emit_store(struct jit *j, int size, u8 base, i32 off, u8 src) {
  switch (size) {
  case 8:
    rex(j, 1, src >= 8, 0, base >= 8, 0);
    e8(j, 0x89);
    break;
  case 4:
    rex(j, 0, src >= 8, 0, base >= 8, 0);
    e8(j, 0x89);
    break;
  case 2:
    e8(j, 0x66);
    rex(j, 0, src >= 8, 0, base >= 8, 0);
    e8(j, 0x89);
    break;
  default:
    /* An 8-bit store from rsi/rdi/rsp/rbp needs a REX prefix to name the low
     * byte of the register rather than ah/ch/dh/bh. */
    rex(j, 0, src >= 8, 0, base >= 8, src >= 4);
    e8(j, 0x88);
    break;
  }
  mem_operand(j, src, base, off);
}

/* *(size *)(base + off) = imm. */
static void emit_store_imm(struct jit *j, int size, u8 base, i32 off, i32 imm) {
  switch (size) {
  case 8:
    rex(j, 1, 0, 0, base >= 8, 0);
    e8(j, 0xC7);
    mem_operand(j, 0, base, off);
    e32(j, (u32)imm);
    break;
  case 4:
    rex(j, 0, 0, 0, base >= 8, 0);
    e8(j, 0xC7);
    mem_operand(j, 0, base, off);
    e32(j, (u32)imm);
    break;
  case 2:
    e8(j, 0x66);
    rex(j, 0, 0, 0, base >= 8, 0);
    e8(j, 0xC7);
    mem_operand(j, 0, base, off);
    e8(j, (u8)imm);
    e8(j, (u8)(imm >> 8));
    break;
  default:
    rex(j, 0, 0, 0, base >= 8, 0);
    e8(j, 0xC6);
    mem_operand(j, 0, base, off);
    e8(j, (u8)imm);
    break;
  }
}

static void emit_prologue(struct jit *j) {
  e8(j, 0x55);                      /* push rbp */
  alu_rr(j, 0x89, 1, 5, 4);         /* mov rbp, rsp */
  alu_ri(j, 5, 1, 4, BPF_STACK_SIZE); /* sub rsp, 512 */
  e8(j, 0x53);                      /* push rbx */
  e8(j, 0x41); e8(j, 0x55);         /* push r13 */
  e8(j, 0x41); e8(j, 0x56);         /* push r14 */
  e8(j, 0x41); e8(j, 0x57);         /* push r15 */
}

static void emit_epilogue(struct jit *j) {
  e8(j, 0x41); e8(j, 0x5F);         /* pop r15 */
  e8(j, 0x41); e8(j, 0x5E);         /* pop r14 */
  e8(j, 0x41); e8(j, 0x5D);         /* pop r13 */
  e8(j, 0x5B);                      /* pop rbx */
  alu_ri(j, 0, 1, 4, BPF_STACK_SIZE); /* add rsp, 512 */
  e8(j, 0x5D);                      /* pop rbp */
  e8(j, 0xC3);                      /* ret */
}

/* The helper call. BPF passes r1..r5; the dispatcher's own first argument is
 * the helper id, so every argument moves up one register. r1..r5 are clobbered
 * by a call in the BPF ABI too, so shuffling them in place is allowed. */
static void emit_helper_call(struct jit *j, u32 id, u64 fn) {
  alu_rr(j, 0x89, 1, 8, 1); /* mov r8, rcx   (arg5 = BPF r4) */
  alu_rr(j, 0x89, 1, 1, 2); /* mov rcx, rdx  (arg4 = BPF r3) */
  alu_rr(j, 0x89, 1, 2, 6); /* mov rdx, rsi  (arg3 = BPF r2) */
  alu_rr(j, 0x89, 1, 6, 7); /* mov rsi, rdi  (arg2 = BPF r1) */
  mov_ri32(j, 0, 7, id);    /* mov edi, id   (arg1 = the helper number) */
  mov_ri64(j, X86_RAX, fn);
  e8(j, 0xFF);
  e8(j, 0xD0); /* call rax — the return value is already BPF r0 */
}

static int jit_cc(u8 op, int *use_test) {
  *use_test = 0;
  switch (op) {
  case BPF_JEQ: return CC_E;
  case BPF_JNE: return CC_NE;
  case BPF_JGT: return CC_A;
  case BPF_JGE: return CC_AE;
  case BPF_JLT: return CC_B;
  case BPF_JLE: return CC_BE;
  case BPF_JSGT: return CC_G;
  case BPF_JSGE: return CC_GE;
  case BPF_JSLT: return CC_L;
  case BPF_JSLE: return CC_LE;
  case BPF_JSET:
    *use_test = 1;
    return CC_NE;
  default: return -1;
  }
}

/* One pass over the program. `offs` is filled in with the code offset of every
 * instruction; when `final` is set the jumps are patched against it. */
static void jit_pass(struct jit *j, const struct bpf_jit_req *req, u32 *offs,
                     int final) {
  j->len = 0;
  emit_prologue(j);
  for (u32 pc = 0; pc < req->insn_cnt && !j->fail; pc++) {
    const struct bpf_insn *in = &req->insns[pc];
    u8 cls = BPF_CLASS(in->code);
    u8 op = BPF_OP(in->code);
    int is_x = BPF_SRC(in->code) == BPF_X;
    int dstb = in->dst_src & 0xf, srcb = (in->dst_src >> 4) & 0xf;
    u8 dst, src;
    int w = (cls == BPF_ALU64 || cls == BPF_JMP) ? 1 : 0;

    offs[pc] = (u32)j->len;
    if (dstb > 10 || srcb > 10) {
      j->fail = 1;
      break;
    }
    dst = jit_reg[dstb];
    src = jit_reg[srcb];

    switch (cls) {
    case BPF_ALU:
    case BPF_ALU64:
      switch (op) {
      case BPF_ADD:
        if (is_x) alu_rr(j, 0x01, w, dst, src); else alu_ri(j, 0, w, dst, (u32)in->imm);
        break;
      case BPF_SUB:
        if (is_x) alu_rr(j, 0x29, w, dst, src); else alu_ri(j, 5, w, dst, (u32)in->imm);
        break;
      case BPF_OR:
        if (is_x) alu_rr(j, 0x09, w, dst, src); else alu_ri(j, 1, w, dst, (u32)in->imm);
        break;
      case BPF_AND:
        if (is_x) alu_rr(j, 0x21, w, dst, src); else alu_ri(j, 4, w, dst, (u32)in->imm);
        break;
      case BPF_XOR:
        if (is_x) alu_rr(j, 0x31, w, dst, src); else alu_ri(j, 6, w, dst, (u32)in->imm);
        break;
      case BPF_MUL:
        if (is_x) imul_rr(j, w, dst, src); else imul_ri(j, w, dst, (u32)in->imm);
        break;
      case BPF_MOV:
        if (is_x) alu_rr(j, 0x89, w, dst, src); else mov_ri32(j, w, dst, (u32)in->imm);
        break;
      case BPF_NEG:
        grp3(j, 3, w, dst);
        break;
      case BPF_LSH:
      case BPF_RSH:
      case BPF_ARSH: {
        u8 digit = op == BPF_LSH ? 4 : (op == BPF_RSH ? 5 : 7);

        if (!is_x) {
          shift_ri(j, digit, w, dst, (u8)(in->imm & (w ? 63 : 31)));
        } else if (src == X86_RCX) {
          shift_cl(j, digit, w, dst);
        } else if (dst == X86_RCX) {
          j->fail = 1; /* the count register is the target: not translated */
        } else {
          e8(j, 0x51); /* push rcx */
          alu_rr(j, 0x89, 1, X86_RCX, src);
          shift_cl(j, digit, w, dst);
          e8(j, 0x59); /* pop rcx */
        }
        break;
      }
      default:
        /* Division, remainder and the byte swaps: the interpreter's. */
        j->fail = 1;
        break;
      }
      break;

    case BPF_LDX: {
      int size = BPF_SIZE(in->code) == BPF_B   ? 1
                 : BPF_SIZE(in->code) == BPF_H ? 2
                 : BPF_SIZE(in->code) == BPF_W ? 4
                                              : 8;

      emit_load(j, size, dst, src, in->off);
      break;
    }

    case BPF_ST:
    case BPF_STX: {
      int size = BPF_SIZE(in->code) == BPF_B   ? 1
                 : BPF_SIZE(in->code) == BPF_H ? 2
                 : BPF_SIZE(in->code) == BPF_W ? 4
                                              : 8;

      if (cls == BPF_ST)
        emit_store_imm(j, size, dst, in->off, in->imm);
      else
        emit_store(j, size, dst, in->off, src);
      break;
    }

    case BPF_LD:
      if (in->code == (BPF_LD | BPF_DW | BPF_IMM)) { /* LD_DW_IMM */
        u64 v;

        if (srcb == 1) {
          int idx = in->off;

          v = (idx >= 0 && idx < req->nmaps) ? (u64)(usize)req->maps[idx] : 0;
        } else {
          v = (u64)(u32)in->imm |
              ((u64)(u32)req->insns[pc + 1].imm << 32);
        }
        mov_ri64(j, dst, v);
        /* The second half of the wide load is not an instruction of its own;
         * give it this one's offset so a jump to it lands here. */
        if (pc + 1 < req->insn_cnt)
          offs[pc + 1] = offs[pc];
        pc++;
        break;
      }
      j->fail = 1;
      break;

    case BPF_JMP:
    case BPF_JMP32: {
      if (op == BPF_EXIT) {
        emit_epilogue(j);
        break;
      }
      if (op == BPF_CALL) {
        emit_helper_call(j, (u32)in->imm, req->helper_fn);
        break;
      }
      {
        int use_test = 0;
        int cc = op == BPF_JA ? -2 : jit_cc(op, &use_test);
        i64 target = (i64)pc + 1 + in->off;

        if (cc == -1 || target < 0 || target >= (i64)req->insn_cnt) {
          j->fail = 1;
          break;
        }
        if (cc != -2) {
          int cw = (cls == BPF_JMP) ? 1 : 0;

          if (use_test) {
            if (is_x)
              alu_rr(j, 0x85, cw, dst, src);
            else {
              grp3(j, 0, cw, dst);
              e32(j, (u32)in->imm);
            }
          } else if (is_x) {
            alu_rr(j, 0x39, cw, dst, src);
          } else {
            alu_ri(j, 7, cw, dst, (u32)in->imm);
          }
          e8(j, 0x0F);
          e8(j, (u8)(0x80 | cc));
        } else {
          e8(j, 0xE9);
        }
        {
          u32 here = (u32)j->len + 4; /* the rel32 is relative to the next insn */
          i32 rel = final ? (i32)(offs[target] - here) : 0;

          e32(j, (u32)rel);
        }
      }
      break;
    }

    default:
      j->fail = 1;
      break;
    }
  }
  /* A program whose last instruction is not EXIT would run off the end; the
   * verifier refuses that, and the epilogue here makes it a return in any
   * case. */
  emit_epilogue(j);
}

void *bpf_jit_compile(const struct bpf_jit_req *req, usize *out_len) {
  struct jit j;
  u32 *offs;
  usize cap;
  void *code;

  if (!req || !req->insn_cnt || !req->helper_fn)
    return 0;
  /* Enough room for the longest translation of every instruction (a wide move
   * plus a shuffle is well under 64 bytes), plus the two epilogues. */
  cap = (usize)req->insn_cnt * 64 + 128;
  offs = kzalloc((usize)(req->insn_cnt + 1) * sizeof(u32));
  if (!offs)
    return 0;
  code = module_alloc(cap);
  if (!code) {
    kfree(offs);
    return 0;
  }
  memset(&j, 0, sizeof(j));
  j.buf = code;
  j.cap = cap;
  /* Two passes: the first one fixes every instruction's address (the encodings
   * are fixed-length, so the second pass lays out identically), the second one
   * writes the jumps against it. */
  jit_pass(&j, req, offs, 0);
  if (!j.fail)
    jit_pass(&j, req, offs, 1);
  if (j.fail) {
    module_free(code);
    kfree(offs);
    return 0;
  }
  kfree(offs);
  if (module_set_prot(code, cap, MODULE_PROT_RX) < 0) {
    module_free(code);
    return 0;
  }
  if (out_len)
    *out_len = cap;
  return code;
}

void bpf_jit_free(void *code) {
  if (code)
    module_free(code);
}

#else /* not x86_64 */

void *bpf_jit_compile(const struct bpf_jit_req *req, usize *out_len) {
  (void)req;
  (void)out_len;
  return 0; /* aarch64: the interpreter, until there is an emitter for it */
}

void bpf_jit_free(void *code) { (void)code; }

#endif
