#ifndef B1NIX_KLOG_H
#define B1NIX_KLOG_H

#include <b1nix/types.h>

/* Log levels */
#define KLOG_DEBUG 0
#define KLOG_INFO  1
#define KLOG_WARN  2
#define KLOG_ERROR 3
#define KLOG_PANIC 4

#define KLOG_BUF_SIZE 65536

/* Core logging functions */
void klog_debug(const char *msg);
/* Runtime-filtered diagnostics, enabled with b1nix.debug=all or a category. */
int klog_debug_enabled(const char *category);
void klog_debug_category(const char *category, const char *msg);
void klog_info(const char *msg);
void klog_warn(const char *msg);
void klog_error(const char *msg);

/* Capture one console character into the ring (called from console_putc). */
void klog_putc(char ch);

/* Ring buffer access for userspace (dmesg) */
usize klog_read(char *buf, usize max_len);
usize klog_size(void);

/* Secondary, cursor-based consumer of the same ring (M98 netconsole).
 *
 * klog_read is a "give me the tail" call shared by every dmesg reader and must
 * not consume. A log shipper instead keeps its own cursor: klog_cursor_now()
 * returns the current write position, and klog_drain copies everything written
 * since *cursor into buf (NUL-terminated), advancing *cursor by what it took.
 * Best-effort, like every netconsole: if the writer laps the cursor while the
 * drain thread is descheduled, the lapped bytes are lost rather than resent. */
usize klog_cursor_now(void);
usize klog_drain(usize *cursor, char *buf, usize max_len);

/* Dump the most recent log messages before a crash/panic. */
void klog_dump_recent(usize max_bytes);

/* Symbol table for backtraces */
void panic_backtrace(void);

/* kallsyms: resolve a kernel text address to "name"+offset (M35). Returns the
 * symbol name (or NULL) and sets *off to the byte offset into it. ksym_print
 * emits " <name+0xoff>" to console+serial when the address resolves. */
const char *ksym_lookup(u64 addr, u64 *off);
void ksym_print(u64 addr);

/* Diagnostic dump helpers */
void describe_address(u64 addr);
void dump_code_around_pc(u64 pc);
void dump_raw_stack_with_symbols(u64 sp, usize num_words);
void dump_recent_syscalls(void);
void dump_smp_cpus_state(void);
void dump_memory_summary(void);
void dump_uptime_summary(void);
struct task;
void dump_task_fds(struct task *t);
void dump_task_vmas(struct task *t);
void dump_task_signals(struct task *t);

/* Enhanced panic (defined in panic.h, implemented in klog.c) */



void panic_at(const char *message, const char *file, int line) __attribute__((noreturn));
void panic(const char *message) __attribute__((noreturn));

#define panic(msg) panic_at((msg), __FILE__, __LINE__)

#define ASSERT(condition) \
	do { \
		if (!(condition)) { \
			panic_at("assertion failed: " #condition, __FILE__, __LINE__); \
		} \
	} while (0)

#endif

