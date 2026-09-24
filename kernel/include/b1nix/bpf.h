/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * eBPF — programs the kernel runs on behalf of userspace (M126).
 *
 * A program is verified, loaded, and then run by the kernel at a place the
 * loader chose: here, on every sample a perf event takes. It can keep state in
 * maps that userspace reads while it runs, which is what makes a profiler a
 * histogram rather than a firehose of records.
 *
 * WHAT IS HERE AND WHAT IS NOT
 *
 * The instruction set is the whole of eBPF's arithmetic, jumps, memory access
 * and calls, run by an interpreter. The verifier is real -- it walks every
 * path, tracks what each register holds, and refuses anything it cannot prove
 * safe -- but it is a SUBSET of Linux's: no loops (a program must be a DAG, as
 * Linux itself required before bounded loops), no pointer arithmetic beyond a
 * constant offset from a known base, and no program types beyond the tracing
 * ones below. Programs are JIT-compiled where the JIT can translate them and
 * interpreted otherwise. The kernel's BTF is at /sys/kernel/btf/vmlinux, so a
 * CO-RE loader can relocate against it, and a program's own BTF is loaded and
 * checked with BPF_BTF_LOAD (kernel/bpf/bpf_btf.c). Anything else is stated
 * to the caller as an error at load time rather than discovered at run time.
 *
 * The constants are Linux's UAPI values; they are ABI.
 */
#ifndef B1NIX_BPF_H
#define B1NIX_BPF_H

#include <b1nix/types.h>

/* bpf(2) commands. */
#define BPF_MAP_CREATE 0
#define BPF_MAP_LOOKUP_ELEM 1
#define BPF_MAP_UPDATE_ELEM 2
#define BPF_MAP_DELETE_ELEM 3
#define BPF_MAP_GET_NEXT_KEY 4
#define BPF_PROG_LOAD 5
#define BPF_OBJ_PIN 6
#define BPF_OBJ_GET 7
#define BPF_PROG_ATTACH 8
#define BPF_PROG_DETACH 9
#define BPF_PROG_TEST_RUN 10
#define BPF_PROG_GET_NEXT_ID 11
#define BPF_MAP_GET_NEXT_ID 12
#define BPF_PROG_GET_FD_BY_ID 13
#define BPF_MAP_GET_FD_BY_ID 14
#define BPF_OBJ_GET_INFO_BY_FD 15
#define BPF_BTF_LOAD 18
#define BPF_BTF_GET_FD_BY_ID 19
#define BPF_BTF_GET_NEXT_ID 23
#define BPF_LINK_CREATE 28
#define BPF_LINK_UPDATE 29
#define BPF_LINK_GET_FD_BY_ID 30
#define BPF_LINK_GET_NEXT_ID 31
#define BPF_LINK_DETACH 34

/* Attach and link types (BPF_LINK_CREATE). */
#define BPF_PERF_EVENT 41
#define BPF_LINK_TYPE_PERF_EVENT 7

/* Map types. */
#define BPF_MAP_TYPE_UNSPEC 0
#define BPF_MAP_TYPE_HASH 1
#define BPF_MAP_TYPE_ARRAY 2
#define BPF_MAP_TYPE_PERF_EVENT_ARRAY 4
#define BPF_MAP_TYPE_PERCPU_HASH 5
#define BPF_MAP_TYPE_PERCPU_ARRAY 6
#define BPF_MAP_TYPE_STACK_TRACE 7
#define BPF_MAP_TYPE_RINGBUF 27

/* bpf_get_stackid / bpf_get_stack flags. */
#define BPF_F_SKIP_FIELD_MASK 0xffULL
#define BPF_F_USER_STACK (1ULL << 8)
#define BPF_F_FAST_STACK_CMP (1ULL << 9)
#define BPF_F_REUSE_STACKID (1ULL << 10)
#define BPF_F_USER_BUILD_ID (1ULL << 11)

/* Ring buffer: flags, and what bpf_ringbuf_query asks. */
/* perf_event_output: the index is the CPU the program runs on. */
#define BPF_F_INDEX_MASK 0xffffffffULL
#define BPF_F_CURRENT_CPU BPF_F_INDEX_MASK

#define BPF_RB_NO_WAKEUP (1ULL << 0)
#define BPF_RB_FORCE_WAKEUP (1ULL << 1)
#define BPF_RB_AVAIL_DATA 0
#define BPF_RB_RING_SIZE 1
#define BPF_RB_CONS_POS 2
#define BPF_RB_PROD_POS 3
#define BPF_RINGBUF_BUSY_BIT (1U << 31)
#define BPF_RINGBUF_DISCARD_BIT (1U << 30)
#define BPF_RINGBUF_HDR_SZ 8

/* Program types. */
#define BPF_PROG_TYPE_UNSPEC 0
#define BPF_PROG_TYPE_SOCKET_FILTER 1
#define BPF_PROG_TYPE_KPROBE 2
#define BPF_PROG_TYPE_TRACEPOINT 5
#define BPF_PROG_TYPE_PERF_EVENT 7

/* Update flags. */
#define BPF_ANY 0
#define BPF_NOEXIST 1
#define BPF_EXIST 2

/* Helper function ids, as the ABI numbers them. */
#define BPF_FUNC_map_lookup_elem 1
#define BPF_FUNC_map_update_elem 2
#define BPF_FUNC_map_delete_elem 3
#define BPF_FUNC_probe_read 4
#define BPF_FUNC_ktime_get_ns 5
#define BPF_FUNC_trace_printk 6
#define BPF_FUNC_get_smp_processor_id 8
#define BPF_FUNC_get_current_pid_tgid 14
#define BPF_FUNC_get_current_uid_gid 15
#define BPF_FUNC_get_current_comm 16
#define BPF_FUNC_perf_event_output 25
#define BPF_FUNC_get_stackid 27
#define BPF_FUNC_get_current_task 35
#define BPF_FUNC_probe_read_str 45
#define BPF_FUNC_get_stack 67
#define BPF_FUNC_probe_read_user 112
#define BPF_FUNC_probe_read_kernel 113
#define BPF_FUNC_probe_read_user_str 114
#define BPF_FUNC_probe_read_kernel_str 115
#define BPF_FUNC_ktime_get_boot_ns 125
#define BPF_FUNC_ringbuf_output 130
#define BPF_FUNC_ringbuf_reserve 131
#define BPF_FUNC_ringbuf_submit 132
#define BPF_FUNC_ringbuf_discard 133
#define BPF_FUNC_ringbuf_query 134

/* The instruction, exactly as the ABI lays it out. */
struct bpf_insn {
  u8 code;
  u8 dst_src; /* dst in the low nibble, src in the high one */
  i16 off;
  i32 imm;
};

/* bpf(2)'s union, in the shape the commands used here read it. Linux's
 * bpf_attr is one union of many structures; a caller passes the size of the
 * part it filled in, so a kernel that reads fewer bytes still works. */
union bpf_attr {
  struct { /* BPF_MAP_CREATE */
    u32 map_type;
    u32 key_size;
    u32 value_size;
    u32 max_entries;
    u32 map_flags;
    u32 inner_map_fd;
    u32 numa_node;
    char map_name[16];
  } create;
  struct { /* the element commands */
    u32 map_fd;
    u32 pad;
    u64 key;
    union {
      u64 value;
      u64 next_key;
    };
    u64 flags;
  } elem;
  struct { /* BPF_PROG_LOAD */
    u32 prog_type;
    u32 insn_cnt;
    u64 insns;
    u64 license;
    u32 log_level;
    u32 log_size;
    u64 log_buf;
    u32 kern_version;
    u32 prog_flags;
    char prog_name[16];
    u32 prog_ifindex;
    u32 expected_attach_type;
    u32 prog_btf_fd;
    u32 func_info_rec_size;
    u64 func_info;
    u32 func_info_cnt;
    u32 line_info_rec_size;
    u64 line_info;
    u32 line_info_cnt;
    u32 attach_btf_id;
    u32 attach_prog_fd;
    u32 core_relo_cnt;
    u64 fd_array;
    u64 core_relos;
    u32 core_relo_rec_size;
    u32 log_true_size;
  } load;
  struct { /* BPF_BTF_LOAD */
    u64 btf;
    u64 btf_log_buf;
    u32 btf_size;
    u32 btf_log_size;
    u32 btf_log_level;
    u32 btf_log_true_size;
    u32 btf_flags;
    u32 btf_token_fd;
  } btf_load;
  struct { /* the *_GET_FD_BY_ID and *_GET_NEXT_ID commands */
    u32 start_id; /* btf_id, prog_id, map_id: one word, several names */
    u32 next_id;
    u32 open_flags;
  } getid;
  struct { /* BPF_OBJ_GET_INFO_BY_FD */
    u32 bpf_fd;
    u32 info_len;
    u64 info;
  } info;
  struct { /* BPF_LINK_CREATE */
    u32 prog_fd;
    u32 target_fd;
    u32 attach_type;
    u32 flags;
  } link_create;
  struct { /* BPF_LINK_UPDATE */
    u32 link_fd;
    u32 new_prog_fd;
    u32 flags;
    u32 old_prog_fd;
  } link_update;
  struct { /* BPF_LINK_DETACH */
    u32 link_fd;
  } link_detach;
  struct { /* BPF_PROG_TEST_RUN */
    u32 prog_fd;
    u32 retval;
    u32 data_size_in;
    u32 data_size_out;
    u64 data_in;
    u64 data_out;
    u32 repeat;
    u32 duration;
    u32 ctx_size_in;
    u32 ctx_size_out;
    u64 ctx_in;
    u64 ctx_out;
  } test;
};

/* What BPF_OBJ_GET_INFO_BY_FD reports for a BTF object (Linux's layout). */
struct bpf_btf_info {
  u64 btf;
  u32 btf_size;
  u32 id;
  u64 name;
  u32 name_len;
  u32 kernel_btf;
};

/* A BTF object: the kernel's own (kernel = 1, never freed) or one a loader
 * handed in with BPF_BTF_LOAD. */
struct bpf_btf {
  int used;
  int kernel;
  int refs;
  u32 id;
  const u8 *data;
  u8 *owned; /* the copy freed with the object; 0 for the kernel's */
  u32 size;
  u32 ntypes;
};

int btf_parse(const u8 *data, u32 size, u32 *ntypes_out, char *log,
              u32 log_size);
u32 btf_type_kind(const u8 *data, u32 id);
u32 btf_str_len(const u8 *data);
struct bpf_btf *btf_vmlinux(void);
struct bpf_btf *btf_new(u8 *data, u32 size);
struct bpf_btf *btf_by_id(u32 id);
void btf_get(struct bpf_btf *b);
void btf_put(struct bpf_btf *b);
u32 btf_next_id(u32 after);
struct vfs_node;
isize btf_vmlinux_read(struct vfs_node *node, u64 offset, char *buf,
                       usize size, int flags);
usize btf_vmlinux_size(void);

/* The system-call hook: returns 1 and sets *ret when `nr` is bpf(2)'s. */
int bpf_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 *ret);

/* Attach a loaded program to a perf event, by descriptor. Returns 0, or a
 * negative errno. `prog_fd` < 0 detaches. Called from
 * PERF_EVENT_IOC_SET_BPF. */
void *bpf_prog_get(int prog_fd);
void bpf_prog_put(void *prog);

/* Run an attached program on one perf sample. `ctx` is the sampled
 * instruction pointer; the return value is the program's, which a profiler
 * uses to decide whether to keep the sample. Runs in interrupt context: the
 * interpreter allocates nothing and takes no lock that a task path holds while
 * it sleeps. */
/* Run a program on a sampling event: `frame` is the interrupted context
 * (struct interrupt_frame), the program's bpf_perf_event_data is built from
 * it. */
u64 bpf_run_perf(void *prog, const void *frame, int in_user, u64 period);

/* Run a program on a tracepoint or kprobe hit. A tracepoint program gets the
 * site's record as tracefs's `format` describes it; a kprobe program gets the
 * probed context's pt_regs, from `frame` (the int3's interrupt frame). */
u64 bpf_run_trace(void *prog, u16 id, u64 a, u64 b, u64 c, const void *frame);

/* The program's type, for the attach checks: a sampling event takes a
 * PERF_EVENT program, a tracepoint a TRACEPOINT one, a kprobe a KPROBE one. */
u32 bpf_prog_type(void *prog);

/* Whether a program run since the last call committed a ring-buffer record
 * that wants its readers woken. The caller wakes them once it holds no lock
 * a woken reader could want. */
int bpf_take_wakeup(void);

/* The perf side of a bpf_link (kernel/perf/perf_event.c). */
struct vfs_handle;
int perf_event_bpf_attach(struct vfs_handle *h, void *prog);
void perf_event_bpf_detach(struct vfs_handle *h, void *prog);
int perf_event_bpf_output(struct vfs_handle *h, const void *data, u32 size,
                          int cpu);
int perf_event_is_handle(struct vfs_handle *h);

/* ── the JIT (kernel/bpf/bpf_jit_x86.c) ─────────────────────────────────────
 *
 * Translates a verified program to machine code once, at load time. A program
 * containing an instruction the translator does not emit is not compiled and
 * runs on the interpreter, so the JIT changes how fast a program runs and
 * never whether it loads.
 */
struct bpf_insn;

struct bpf_jit_req {
  const struct bpf_insn *insns;
  u32 insn_cnt;
  void *const *maps; /* map pointers by index, for the wide map load */
  int nmaps;
  u64 helper_fn; /* u64 (*)(u32 id, u64, u64, u64, u64, u64) */
};

/* The compiled function, callable as u64 (*)(void *ctx), or NULL. */
void *bpf_jit_compile(const struct bpf_jit_req *req, usize *out_len);
void bpf_jit_free(void *code);

#endif
