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
void klog_info(const char *msg);
void klog_warn(const char *msg);
void klog_error(const char *msg);

/* Capture one console character into the ring (called from console_putc). */
void klog_putc(char ch);

/* Ring buffer access for userspace (dmesg) */
usize klog_read(char *buf, usize max_len);
usize klog_size(void);

/* Symbol table for backtraces */
void klog_register_symbol(u64 address, const char *name);
void panic_backtrace(void);

/* kallsyms: resolve a kernel text address to "name"+offset (M35). Returns the
 * symbol name (or NULL) and sets *off to the byte offset into it. ksym_print
 * emits " <name+0xoff>" to console+serial when the address resolves. */
const char *ksym_lookup(u64 addr, u64 *off);
void ksym_print(u64 addr);

/* Enhanced panic (defined in panic.h, implemented in klog.c) */
void panic(const char *message) __attribute__((noreturn));

#define ASSERT(condition) \
	do { \
		if (!(condition)) { \
			panic("assertion failed: " #condition); \
		} \
	} while (0)

#endif
