#include <string.h>
#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/serial.h>
#include <b1nix/klog.h>

/* ── Kernel log ring buffer ── */
#define KLOG_BUF_SIZE 4096

static const char *klog_level_names[] = {
	"DEBUG", "INFO", "WARN", "ERROR", "PANIC"
};

static char klog_buf[KLOG_BUF_SIZE];
static usize klog_write_pos;
static usize klog_read_pos;
static int klog_overflow;

/* ── Symbol table for backtraces ── */
#define MAX_SYMBOLS 128

struct kernel_symbol {
	u64 address;
	char name[64];
};

static struct kernel_symbol symbol_table[MAX_SYMBOLS];
static int symbol_count;

void klog_register_symbol(u64 address, const char *name)
{
	if (symbol_count >= MAX_SYMBOLS) return;
	symbol_table[symbol_count].address = address;
	usize len = strlen(name);
	if (len > 63) len = 63;
	memcpy(symbol_table[symbol_count].name, name, len);
	symbol_table[symbol_count].name[len] = '\0';
	symbol_count++;
}

static const char *klog_lookup_symbol(u64 address)
{
	const char *best_name = 0;
	u64 best_diff = (u64)-1;

	for (int i = 0; i < symbol_count; i++) {
		if (symbol_table[i].address <= address) {
			u64 diff = address - symbol_table[i].address;
			if (diff < best_diff) {
				best_diff = diff;
				best_name = symbol_table[i].name;
			}
		}
	}

	return best_name;
}

/* ── Core log function ── */
static void klog_write(int level, const char *message)
{
	const char *prefix = klog_level_names[level];
	usize prefix_len = strlen(prefix);
	usize msg_len = strlen(message);

	/* Write to console and serial */
	if (level >= KLOG_WARN) {
		console_write("[");
		console_write(prefix);
		console_write("] ");
		console_write(message);
		console_write("\n");

		serial_write("[");
		serial_write(prefix);
		serial_write("] ");
		serial_write(message);
		serial_write("\n");
	}

	/* Write to ring buffer */
	for (usize i = 0; i < prefix_len && klog_write_pos < KLOG_BUF_SIZE; i++) {
		klog_buf[(klog_write_pos++) % KLOG_BUF_SIZE] = prefix[i];
	}
	if (klog_write_pos < KLOG_BUF_SIZE)
		klog_buf[(klog_write_pos++) % KLOG_BUF_SIZE] = ':';
	if (klog_write_pos < KLOG_BUF_SIZE)
		klog_buf[(klog_write_pos++) % KLOG_BUF_SIZE] = ' ';
	if (klog_write_pos >= KLOG_BUF_SIZE) klog_overflow = 1;

	for (usize i = 0; i < msg_len; i++) {
		if (klog_write_pos < KLOG_BUF_SIZE)
			klog_buf[(klog_write_pos++) % KLOG_BUF_SIZE] = message[i];
		else
			klog_overflow = 1;
	}

	if (klog_write_pos < KLOG_BUF_SIZE)
		klog_buf[(klog_write_pos++) % KLOG_BUF_SIZE] = '\n';
	else
		klog_overflow = 1;

	/* Adjust read position if we overflowed */
	if (klog_overflow) {
		klog_read_pos = klog_write_pos - KLOG_BUF_SIZE;
		if (klog_read_pos >= KLOG_BUF_SIZE)
			klog_read_pos = 0;
	}
}

void klog_debug(const char *msg) { klog_write(KLOG_DEBUG, msg); }
void klog_info(const char *msg)  { klog_write(KLOG_INFO, msg); }
void klog_warn(const char *msg)  { klog_write(KLOG_WARN, msg); }
void klog_error(const char *msg) { klog_write(KLOG_ERROR, msg); }

/* ── Read ring buffer for userspace ── */
usize klog_read(char *buf, usize max_len)
{
	if (max_len == 0) return 0;

	usize available;
	if (klog_write_pos >= klog_read_pos) {
		available = klog_write_pos - klog_read_pos;
	} else {
		available = KLOG_BUF_SIZE - klog_read_pos + klog_write_pos;
	}
	if (klog_overflow && available > KLOG_BUF_SIZE)
		available = KLOG_BUF_SIZE;

	if (available == 0) return 0;
	if (available > max_len - 1) available = max_len - 1;

	for (usize i = 0; i < available; i++) {
		buf[i] = klog_buf[(klog_read_pos + i) % KLOG_BUF_SIZE];
	}
	buf[available] = '\0';

	klog_read_pos = (klog_read_pos + available) % KLOG_BUF_SIZE;
	if (klog_read_pos == klog_write_pos) {
		klog_read_pos = 0;
		klog_write_pos = 0;
		klog_overflow = 0;
	}

	return available;
}

/* ── Get total log size (for userspace query) ── */
usize klog_size(void)
{
	if (klog_write_pos >= klog_read_pos)
		return klog_write_pos - klog_read_pos;
	return KLOG_BUF_SIZE - klog_read_pos + klog_write_pos;
}

/* ── Enhanced panic with backtrace ── */
void panic_backtrace(void)
{
	u64 *rbp = 0;
#ifdef __aarch64__
	__asm__ volatile("mov %0, x29" : "=r"(rbp));
#else
	__asm__ volatile("movq %%rbp, %0" : "=r"(rbp));
#endif

	console_write("\n--- Kernel Backtrace ---\n");
	serial_write("\n--- Kernel Backtrace ---\n");

	int depth = 0;
#ifdef __aarch64__
	/* AArch64: walk frame pointer chain (FP = x29, LR = FP+8) */
	while (rbp && depth < 16) {
		u64 fp = rbp[0];
		u64 lr = rbp[1];

		console_write("  #");
		console_write_dec(depth);
		console_write(" 0x");
		console_write_hex64(lr);

		const char *name = klog_lookup_symbol(lr);
		if (name) {
			console_write(" <");
			console_write(name);
			u64 sym_addr = 0;
			for (int i = 0; i < symbol_count; i++) {
				if (symbol_table[i].address <= lr &&
				    symbol_table[i].address > sym_addr)
					sym_addr = symbol_table[i].address;
			}
			if (sym_addr && lr > sym_addr) {
				console_write("+0x");
				console_write_hex64(lr - sym_addr);
			}
			console_write(">");
		}
		console_write("\n");

		if (fp == 0 || fp <= (u64)(usize)rbp) break;
		rbp = (u64 *)(usize)fp;
		depth++;
	}
#else
	/* x86_64: walk frame pointer chain */
	while (rbp && depth < 16) {
		u64 rip = rbp[1];
		u64 new_rbp = rbp[0];

		if (rip == 0 || rip < 0xFFFF800000000000ULL) break;
		if (new_rbp != 0 && new_rbp <= (u64)(usize)rbp) break;

		console_write("  #");
		console_write_dec(depth);
		console_write(" 0x");
		console_write_hex64(rip);

		const char *name = klog_lookup_symbol(rip);
		if (name) {
			console_write(" <");
			console_write(name);
			console_write(">");
		}
		console_write("\n");

		rbp = (u64 *)(usize)new_rbp;
		depth++;
	}
#endif

	console_write("--- End Backtrace ---\n\n");
}

void panic(const char *message)
{
	klog_write(KLOG_PANIC, message);
	panic_backtrace();
	arch_halt();
}
