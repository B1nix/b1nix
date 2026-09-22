/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_BPF_INSN_H
#define B1NIX_BPF_INSN_H

/*
 * The eBPF instruction encoding, and the limits this kernel puts on a program.
 *
 * ABI values, shared by everything that has to read an instruction: the
 * verifier and interpreter in kernel/bpf/bpf_core.c, and the translator in
 * kernel/bpf/bpf_jit_x86.c.
 */

#define BPF_CLASS(code) ((code) & 0x07)
#define BPF_LD 0x00
#define BPF_LDX 0x01
#define BPF_ST 0x02
#define BPF_STX 0x03
#define BPF_ALU 0x04
#define BPF_JMP 0x05
#define BPF_JMP32 0x06
#define BPF_ALU64 0x07

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
#define BPF_MOV 0xb0
#define BPF_ARSH 0xc0
#define BPF_END 0xd0

#define BPF_JA 0x00
#define BPF_JEQ 0x10
#define BPF_JGT 0x20
#define BPF_JGE 0x30
#define BPF_JSET 0x40
#define BPF_JNE 0x50
#define BPF_JSGT 0x60
#define BPF_JSGE 0x70
#define BPF_CALL 0x80
#define BPF_EXIT 0x90
#define BPF_JLT 0xa0
#define BPF_JLE 0xb0
#define BPF_JSLT 0xc0
#define BPF_JSLE 0xd0

#define BPF_SRC(code) ((code) & 0x08)
#define BPF_K 0x00
#define BPF_X 0x08

#define BPF_SIZE(code) ((code) & 0x18)
#define BPF_W 0x00
#define BPF_H 0x08
#define BPF_B 0x10
#define BPF_DW 0x18

#define BPF_IMM 0x00
#define BPF_MEM 0x60

#define INSN_DST(i) ((i)->dst_src & 0x0f)
#define INSN_SRC(i) (((i)->dst_src >> 4) & 0x0f)

#define BPF_REG_CNT 11
#define BPF_STACK_SIZE 512
#define BPF_MAX_INSNS 4096
#define BPF_MAX_MAPS 64
#define BPF_MAX_PROGS 64
#define BPF_PROG_MAX_MAPS 8
/* A program runs on every sample, in interrupt context, so it must finish.
 * The verifier forbids loops; this only bounds a pathological DAG. */
#define BPF_MAX_STEPS 16384

#endif /* B1NIX_BPF_INSN_H */
