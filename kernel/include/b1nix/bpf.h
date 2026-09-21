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
 * ones below. There is no JIT: programs are interpreted. There is no BTF, so
 * CO-RE programs will not load. Each of those is stated to the caller as an
 * error at load time rather than discovered at run time.
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

/* Map types. */
#define BPF_MAP_TYPE_UNSPEC 0
#define BPF_MAP_TYPE_HASH 1
#define BPF_MAP_TYPE_ARRAY 2

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
#define BPF_FUNC_ktime_get_ns 5
#define BPF_FUNC_trace_printk 6
#define BPF_FUNC_get_smp_processor_id 8
#define BPF_FUNC_get_current_pid_tgid 14
#define BPF_FUNC_get_current_uid_gid 15
#define BPF_FUNC_get_current_comm 16
#define BPF_FUNC_ktime_get_boot_ns 125

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
  } load;
  struct { /* BPF_PROG_TEST_RUN */
    u32 prog_fd;
    u32 retval;
    u32 data_size_in;
    u32 data_size_out;
    u64 data_in;
    u64 data_out;
    u32 repeat;
    u32 duration;
  } test;
};

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
u64 bpf_run_perf(void *prog, u64 ip, u64 pid_tgid, u64 cpu);

#endif
