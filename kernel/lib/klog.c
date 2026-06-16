#include <string.h>
#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/serial.h>
#include <b1nix/klog.h>

/* Kernel log ring buffer — KLOG_BUF_SIZE comes from <b1nix/klog.h> (64 KiB) so
 * the whole boot (PCI/driver/dhcp output, fed in via klog_putc from
 * console_putc) survives until the shell, where `dmesg` can read it back. */

static const char *klog_level_names[] = {
	"DEBUG", "INFO", "WARN", "ERROR", "PANIC"
};

static char klog_buf[KLOG_BUF_SIZE];
static usize klog_write_pos;
static usize klog_read_pos;
static int klog_overflow;

/* ── Symbol table for backtraces ── */
#define MAX_SYMBOLS 2048

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

/* ── kallsyms: post-link symbol blob (M35) ──
 * Walks the packed [u64 addr][asciz name] records the two-pass link emitted
 * into the .kallsyms section (see tools/gen_kallsyms.sh, linker.ld). Returns
 * the name of the function containing `addr` and, via *off, the byte offset
 * into it. Names point into the loaded blob, valid for the kernel's lifetime. */
extern const unsigned char __kallsyms_start[];
extern const unsigned char __kallsyms_end[];

const char *ksym_lookup(u64 addr, u64 *off)
{
	const unsigned char *p = __kallsyms_start;
	const unsigned char *end = __kallsyms_end;
	u64 best_addr = 0;
	const char *best_name = 0;

	while (p + 8 < end) {
		u64 a;
		memcpy(&a, p, 8);
		p += 8;
		const char *name = (const char *)p;
		while (p < end && *p)
			p++;
		p++; /* skip the NUL terminator */
		if (a <= addr && a >= best_addr) {
			best_addr = a;
			best_name = name;
		}
	}

	if (best_name && off)
		*off = addr - best_addr;
	return best_name;
}

/* Print " <symbol+0xoff>" to console and serial if `addr` resolves. */
void ksym_print(u64 addr)
{
	u64 off = 0;
	const char *name = ksym_lookup(addr, &off);
	if (!name)
		return;
	console_write(" <");
	console_write(name);
	serial_write(" <");
	serial_write(name);
	if (off) {
		console_write("+0x");
		console_write_hex64(off);
		serial_write("+0x");
	}
	console_write(">");
	serial_write(">");
}

static void klog_ring_put(char ch)
{
    usize next = (klog_write_pos + 1) % KLOG_BUF_SIZE;
    if (next == klog_read_pos) {
        klog_read_pos = (klog_read_pos + 1) % KLOG_BUF_SIZE;
        klog_overflow = 1;
    }
    klog_buf[klog_write_pos] = ch;
    klog_write_pos = next;
}

/* Capture a single console character into the ring. Called from console_putc so
 * that EVERYTHING printed to the screen/serial (driver init, PCI, DHCP, ...) is
 * retrievable later via `dmesg` — the console itself does not scroll back. */
void klog_putc(char ch)
{
    /* The ring is a text log read back via dmesg with C string functions; a
     * stray NUL from console output (e.g. a binary byte printed during a
     * program load) would truncate every reader's view at that point. Drop
     * NULs so the log stays a clean, searchable string. */
    if (ch == '\0')
        return;
    klog_ring_put(ch);
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

    /* Write to circular ring buffer */
    for (usize i = 0; i < prefix_len; i++) {
        klog_ring_put(prefix[i]);
    }
    klog_ring_put(':');
    klog_ring_put(' ');
    for (usize i = 0; i < msg_len; i++) {
        klog_ring_put(message[i]);
    }
    klog_ring_put('\n');
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

	/* Return the MOST RECENT bytes that fit. A small reader (e.g. m15_smoke's
	 * 4 KiB buffer checking for the latest "audit:" lines) then sees the tail it
	 * cares about, while `dmesg`'s full-size (64 KiB) read gets the whole log —
	 * including the early boot driver/PCI/DHCP lines — since the buffer is the
	 * size of the ring. */
	usize start = klog_read_pos;
	if (available > max_len - 1) {
		start = (klog_read_pos + (available - (max_len - 1))) % KLOG_BUF_SIZE;
		available = max_len - 1;
	}

	for (usize i = 0; i < available; i++) {
		buf[i] = klog_buf[(start + i) % KLOG_BUF_SIZE];
	}
	buf[available] = '\0';

	/* Non-destructive: do NOT advance klog_read_pos. `dmesg` returns the full
	 * retained log every time, so the user can grep it repeatedly (and a second
	 * `dmesg | grep ...` isn't empty). The oldest end is still evicted by
	 * klog_ring_put on overflow. */
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
	int depth = 0;
	console_write("\n--- Kernel Backtrace ---\n");
	serial_write("\n--- Kernel Backtrace ---\n");

#ifdef __aarch64__
	u64 *rbp = 0;
	__asm__ volatile("mov %0, x29" : "=r"(rbp));
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
#elif defined(__x86_64__)
	u64 *rbp = 0;
	__asm__ volatile("movq %%rbp, %0" : "=r"(rbp));
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
#else
	u32 *ebp = 0;
	__asm__ volatile("movl %%ebp, %0" : "=r"(ebp));
	/* x86 32-bit: walk frame pointer chain */
	while (ebp && depth < 16) {
		u32 eip = ebp[1];
		u32 new_ebp = ebp[0];

		if (eip == 0) break;
		if (new_ebp != 0 && new_ebp <= (u32)(usize)ebp) break;

		console_write("  #");
		console_write_dec(depth);
		console_write(" 0x");
		console_write_hex64(eip);

		const char *name = klog_lookup_symbol(eip);
		if (name) {
			console_write(" <");
			console_write(name);
			console_write(">");
		}
		console_write("\n");

		ebp = (u32 *)(usize)new_ebp;
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
