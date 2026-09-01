#include <string.h>
#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/serial.h>
#include <b1nix/klog.h>
#include <b1nix/lapic.h>
#include <b1nix/sched.h>
#include <b1nix/lockdep.h>
#include <b1nix/kprintf.h>
#include <b1nix/vfs.h>
#include <b1nix/mm.h>
#include <b1nix/ftrace.h>
#include <b1nix/bootinfo.h>



/* Kernel log ring buffer — KLOG_BUF_SIZE comes from <b1nix/klog.h> (64 KiB) so
 * the whole boot (PCI/driver/dhcp output, fed in via klog_putc from
 * console_putc) survives until the shell, where `dmesg` can read it back. */

static char klog_buf[KLOG_BUF_SIZE];
static usize klog_write_pos;
static usize klog_read_pos;
static int klog_overflow;

/* ── Symbol table for backtraces ── */


/* ── kallsyms: post-link symbol blob (M35) ──
 * Walks the packed [u64 addr][asciz name] records the two-pass link emitted
 * into the .kallsyms section (see tools/kernel/gen_kallsyms.sh, linker.ld). Returns
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

/* Print " <symbol+0xoff>" to console if `addr` resolves. */
void ksym_print(u64 addr)
{
	u64 off = 0;
	const char *name = ksym_lookup(addr, &off);
	if (!name)
		return;
	console_write(" <");
	console_write(name);
	if (off) {
		console_write("+0x");
		console_write_hex64(off);
	}
	console_write(">");
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
/* The legacy level-tagged entry points, expressed in terms of the structured
 * logger so that a klog_warn() line is stamped, filtered and recorded exactly
 * like every other kernel line. The old implementation wrote its own "[WARN] "
 * prefix to the console AND a second copy straight to the UART, which is why
 * warnings used to appear twice in a serial capture. */
static const int klog_syslog_level[] = {
    LOGLEVEL_DEBUG,   /* KLOG_DEBUG */
    LOGLEVEL_INFO,    /* KLOG_INFO  */
    LOGLEVEL_WARNING, /* KLOG_WARN  */
    LOGLEVEL_ERR,     /* KLOG_ERROR */
    LOGLEVEL_EMERG,   /* KLOG_PANIC */
};

static void klog_write_internal(int level, const char *message, int show_debug)
{
    (void)show_debug;
    if (level < KLOG_DEBUG)
        level = KLOG_DEBUG;
    if (level > KLOG_PANIC)
        level = KLOG_PANIC;
    kprintf(klog_syslog_level[level], NULL, "%s", message ? message : "");
}

static void klog_write(int level, const char *message)
{
    klog_write_internal(level, message, 0);
}

static int debug_value_matches(const char *value, const char *category)
{
    if (!value || !category || !category[0])
        return 0;
    if (strcmp(value, "1") == 0 || strcmp(value, "all") == 0)
        return 1;

    usize category_len = strlen(category);
    for (usize i = 0; value[i];) {
        while (value[i] == ',' || value[i] == ' ')
            i++;
        usize start = i;
        while (value[i] && value[i] != ',' && value[i] != ' ')
            i++;
        if (i - start == category_len &&
            memcmp(value + start, category, category_len) == 0)
            return 1;
    }
    return 0;
}

int klog_debug_enabled(const char *category)
{
    char value[96];
    if (bootinfo_get_kv("b1nix.debug", value, sizeof(value)) &&
        debug_value_matches(value, category))
        return 1;

    char flag[64];
    const char prefix[] = "b1nix.debug.";
    usize prefix_len = sizeof(prefix) - 1;
    usize category_len = category ? strlen(category) : 0;
    if (category_len == 0 || prefix_len + category_len + 1 > sizeof(flag))
        return 0;
    memcpy(flag, prefix, prefix_len);
    memcpy(flag + prefix_len, category, category_len);
    flag[prefix_len + category_len] = '\0';
    return bootinfo_has_flag(flag);
}

void klog_debug_category(const char *category, const char *msg)
{
    if (!klog_debug_enabled(category) || !msg)
        return;

    char line[256];
    usize category_len = category ? strlen(category) : 0;
    usize msg_len = strlen(msg);
    if (category_len + msg_len + 3 > sizeof(line))
        msg_len = sizeof(line) - category_len - 3;
    memcpy(line, category, category_len);
    line[category_len] = ':';
    line[category_len + 1] = ' ';
    memcpy(line + category_len + 2, msg, msg_len);
    line[category_len + 2 + msg_len] = '\0';
    klog_write_internal(KLOG_DEBUG, line, 1);
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

/* ── Cursor-based drain (netconsole, M98) ── */
usize klog_cursor_now(void)
{
	return klog_write_pos;
}

usize klog_drain(usize *cursor, char *buf, usize max_len)
{
	if (!cursor || !buf || max_len < 2)
		return 0;

	usize w = klog_write_pos;
	usize c = *cursor % KLOG_BUF_SIZE;
	if (c == w)
		return 0;

	usize available = (w >= c) ? (w - c) : (KLOG_BUF_SIZE - c + w);
	if (available > max_len - 1)
		available = max_len - 1;

	for (usize i = 0; i < available; i++)
		buf[i] = klog_buf[(c + i) % KLOG_BUF_SIZE];
	buf[available] = '\0';

	*cursor = (c + available) % KLOG_BUF_SIZE;
	return available;
}

/* ── Get total log size (for userspace query) ── */
usize klog_size(void)
{
	if (klog_write_pos >= klog_read_pos)
		return klog_write_pos - klog_read_pos;
	return KLOG_BUF_SIZE - klog_read_pos + klog_write_pos;
}

/* ── Enhanced panic with backtrace ──
 * Symbolication goes through ksym_print/ksym_lookup, i.e. the kallsyms blob the
 * two-pass link appends. It used to use klog_lookup_symbol, which reads a small
 * table that code has to register into by hand and which is empty in practice —
 * so every kernel panic on every arch printed a bare address list, and aarch64
 * (no gdbstub here) had nothing else to go on. */
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

		ksym_print(lr);
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

		ksym_print(rip);
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

		ksym_print(eip);
		console_write("\n");

		ebp = (u32 *)(usize)new_ebp;
		depth++;
	}
#endif

	console_write("--- End Backtrace ---\n\n");
}

void klog_dump_recent(usize max_bytes)
{
	if (klog_write_pos == 0 && !klog_overflow)
		return;
	usize available = klog_overflow ? KLOG_BUF_SIZE : klog_write_pos;
	if (max_bytes > available)
		max_bytes = available;
	if (max_bytes == 0)
		return;

	console_write("\n--- Recent Kernel Log Messages ---\n");
	serial_write("\n--- Recent Kernel Log Messages ---\n");

	char tmp[128];
	usize tmp_idx = 0;
	usize start_pos = (klog_write_pos + KLOG_BUF_SIZE - max_bytes) % KLOG_BUF_SIZE;

	for (usize i = 0; i < max_bytes; i++) {
		char ch = klog_buf[(start_pos + i) % KLOG_BUF_SIZE];
		if (ch) {
			tmp[tmp_idx++] = ch;
			if (tmp_idx == sizeof(tmp) - 1) {
				tmp[tmp_idx] = '\0';
				console_write(tmp);
				serial_write(tmp);
				tmp_idx = 0;
			}
		}
	}
	if (tmp_idx > 0) {
		tmp[tmp_idx] = '\0';
		console_write(tmp);
		serial_write(tmp);
	}
	console_write("\n--- End Recent Log ---\n\n");
	serial_write("\n--- End Recent Log ---\n\n");
}

static const char *klog_task_state_str(int st)
{
	switch (st) {
	case 0: return "RUNNING";
	case 1: return "READY";
	case 2: return "BLOCKED";
	case 3: return "SLEEPING";
	case 4: return "STOPPED";
	case 5: return "DEAD";
	case 6: return "REAPING";
	default: return "UNKNOWN";
	}
}

extern char __kernel_text_start[], __kernel_text_end[];
extern char __kernel_start[], __kernel_end[];

void describe_address(u64 addr) {
	if (addr == 0) {
		console_write(" -> NULL POINTER (0x0)");
		return;
	}
	if (addr < 0x1000) {
		console_write(" -> NULL POINTER DEREFERENCE (+0x");
		console_write_hex64(addr);
		console_write(" struct offset)");
		return;
	}
	if (addr >= (u64)(usize)__kernel_text_start && addr < (u64)(usize)__kernel_text_end) {
		console_write(" -> [Kernel Text .text] ");
		ksym_print(addr);
		return;
	}
	if (addr >= (u64)(usize)__kernel_start && addr < (u64)(usize)__kernel_end) {
		console_write(" -> [Kernel Image .rodata/.data/.bss]");
		return;
	}
	struct task *t = current_task;
	if (t && t->stack) {
		u64 sbase = (u64)(usize)t->stack;
		if (addr >= sbase && addr < sbase + 131072) {
			console_write(" -> [Kernel Stack of pid=");
			console_write_dec((u64)t->id);
			console_write(" '");
			console_write(t->name ? t->name : "none");
			console_write("']");
			return;
		}
	}
	if (addr >= 0x1000000000ULL && addr < 0x4000000000ULL) {
		console_write(" -> [Kernel Heap (kheap/arena)]");
		return;
	}
#ifdef __x86_64__
	if (addr >= 0xffffc00000000000ULL) {
		console_write(" -> [Kernel Heap (kheap)]");
		return;
	}
	if (addr >= 0xffff800000000000ULL) {
		console_write(" -> [Kernel Direct Map]");
		return;
	}
#endif
	if (addr >= 0x40000000ULL && addr < 0x100000000ULL) {
		console_write(" -> [Physical RAM Window]");
		return;
	}
	if (addr >= 0x08000000ULL && addr < 0x40000000ULL) {
		console_write(" -> [Device MMIO Space]");
		return;
	}
	if (addr < 0x0000800000000000ULL) {
		console_write(" -> [User Space Virtual Address]");
		return;
	}
	console_write(" -> [High Kernel Address]");
}

void dump_code_around_pc(u64 pc) {
	if (pc < (u64)(usize)__kernel_text_start || pc >= (u64)(usize)__kernel_text_end) {
		return;
	}
	console_write("\nCode around PC (0x");
	console_write_hex64(pc);
	ksym_print(pc);
	console_write("):\n  ");

#if defined(__aarch64__)
	/* AArch64: 32-bit instructions (4-byte aligned) */
	u32 *ptr = (u32 *)(usize)(pc & ~3ULL);
	u32 *start = ptr - 4;
	u32 *end = ptr + 5;
	if ((u64)(usize)start < (u64)(usize)__kernel_text_start) start = (u32 *)(usize)__kernel_text_start;
	if ((u64)(usize)end > (u64)(usize)__kernel_text_end) end = (u32 *)(usize)__kernel_text_end;

	for (u32 *p = start; p < end; p++) {
		if (p == ptr) {
			console_write(" <0x");
			console_write_hex64(*p);
			console_write(">");
		} else {
			console_write(" 0x");
			console_write_hex64(*p);
		}
	}
#else
	/* x86_64: variable length bytes */
	u8 *ptr = (u8 *)(usize)pc;
	u8 *start = ptr - 16;
	u8 *end = ptr + 16;
	if ((u64)(usize)start < (u64)(usize)__kernel_text_start) start = (u8 *)(usize)__kernel_text_start;
	if ((u64)(usize)end > (u64)(usize)__kernel_text_end) end = (u8 *)(usize)__kernel_text_end;

	for (u8 *p = start; p < end; p++) {
		if (p == ptr) {
			console_write(" <0x");
			console_write_hex64(*p);
			console_write(">");
		} else {
			console_write(" 0x");
			console_write_hex64(*p);
		}
	}
#endif
	console_write("\n");
}

void dump_raw_stack_with_symbols(u64 sp, usize num_words) {
	if (sp < 0x1000 || (sp & 7) != 0)
		return;

	console_write("\n--- Raw Kernel Stack (SP=0x");
	console_write_hex64(sp);
	console_write(") ---\n");

	u64 *stack_words = (u64 *)(usize)sp;
	for (usize i = 0; i < num_words; i++) {
		u64 val = stack_words[i];
		console_write("  [SP+0x");
		console_write_hex64((u64)(i * 8));
		console_write("] 0x");
		console_write_hex64(val);
		if (val >= (u64)(usize)__kernel_text_start && val < (u64)(usize)__kernel_text_end) {
			ksym_print(val);
		}
		console_write("\n");
	}
	console_write("--- End Stack ---\n");
}

void dump_smp_cpus_state(void) {
	console_write("\n--- SMP CPU State Snapshot ---\n");
	for (int i = 0; i < MAX_CPUS; i++) {
		struct percpu *pc = get_percpu_n(i);
		if (!pc || !pc->cpu_online) continue;
		console_write("  CPU ");
		console_write_dec((u64)pc->cpu_id);
		struct task *t = pc->cur_task;
		if (t) {
			console_write(": pid=");
			console_write_dec((u64)t->id);
			console_write(" ('");
			console_write(t->name ? t->name : "none");
			console_write("') state=");
			console_write(klog_task_state_str((int)t->state));
			if (t->wait_chan) {
				console_write(" wait=0x");
				console_write_hex64((u64)(usize)t->wait_chan);
				ksym_print((u64)(usize)t->wait_chan);
			}
		} else {
			console_write(": [IDLE / NO TASK]");
		}
		console_write("\n");
	}
	console_write("--- End SMP Snapshot ---\n");
}

void dump_memory_summary(void) {
	console_write("\n--- Memory & Resource Snapshot ---\n");
	usize free_f = pmm_free_frame_count();
	console_write("  PMM Free Frames: ");
	console_write_dec((u64)free_f);
	console_write(" (");
	console_write_dec((u64)(free_f * PAGE_SIZE / (1024 * 1024)));
	console_write(" MB free)\n");

	u64 kh_mapped = kheap_mapped_bytes();
	console_write("  KHeap Footprint: ");
	console_write_dec(kh_mapped / 1024);
	console_write(" kB\n");

	console_write("  Active VFS Inodes/Nodes: ");
	console_write_dec((u64)vfs_active_node_count());
	console_write("\n");
	console_write("--- End Memory Snapshot ---\n");
}


void dump_task_fds(struct task *t) {
	if (!t || !t->fd_table) return;
	console_write("\n--- Task Open File Descriptors (pid=");
	console_write_dec((u64)t->id);
	console_write(") ---\n");
	int printed = 0;
	for (u32 fd = 0; fd < t->fd_capacity && fd < 32; fd++) {
		struct vfs_handle *h = t->fd_table[fd];
		if (!h || !h->used) continue;
		printed++;
		console_write("  fd ");
		console_write_dec((u64)fd);
		console_write(": ");
		if (h->open_path) {
			console_write(h->open_path);
		} else if (h->node && h->node->name[0]) {
			console_write(h->node->name);
		} else {
			switch (h->kind) {
			case VFS_HANDLE_PIPE_READ: console_write("pipe:[r]"); break;
			case VFS_HANDLE_PIPE_WRITE: console_write("pipe:[w]"); break;
			case VFS_HANDLE_SOCKET: console_write("socket:[]"); break;
			case VFS_HANDLE_EVENTFD: console_write("eventfd:[]"); break;
			case VFS_HANDLE_TIMERFD: console_write("timerfd:[]"); break;
			case VFS_HANDLE_SIGNALFD: console_write("signalfd:[]"); break;
			case VFS_HANDLE_EPOLL: console_write("epoll:[]"); break;
			case VFS_HANDLE_INOTIFY: console_write("inotify:[]"); break;
			default: console_write("anon:[]"); break;
			}
		}
		console_write(" (ref=");
		console_write_dec((u64)h->refcount);
		console_write(" off=");
		console_write_dec((u64)h->offset);
		console_write(")\n");
	}
	if (!printed) {
		console_write("  (no open files)\n");
	}
	console_write("--- End FD Table ---\n");
}

void dump_task_vmas(struct task *t) {
	if (!t || !t->vma_list) return;
	console_write("\n--- Task Memory Map VMAs (pid=");
	console_write_dec((u64)t->id);
	console_write(") ---\n");
	int count = 0;
	for (struct vm_area *vma = t->vma_list; vma && count < 16; vma = vma->next, count++) {
		console_write("  0x");
		console_write_hex64(vma->start);
		console_write(" - 0x");
		console_write_hex64(vma->end);
		console_write(" ");
		console_write((vma->prot & 1) ? "r" : "-");
		console_write((vma->prot & 2) ? "w" : "-");
		console_write((vma->prot & 4) ? "x" : "-");
		console_write((vma->flags & 0x02) ? "p" : "s");
		if (vma->node && vma->node->name[0]) {
			console_write(" ");
			console_write(vma->node->name);
		}
		console_write("\n");
	}
	console_write("--- End Memory Map ---\n");
}


void dump_task_signals(struct task *t) {
	if (!t) return;
	console_write("\n--- Task Signal State (pid=");
	console_write_dec((u64)t->id);
	console_write(") ---\n");
	console_write("  Pending: 0x");
	console_write_hex64(t->pending_signals);
	console_write(" | Mask/Blocked: 0x");
	console_write_hex64(t->blocked_signals);
	if (t->last_stop_signal) {
		console_write(" | Last Stop Signal: ");
		console_write_dec((u64)t->last_stop_signal);
	}
	console_write("\n--- End Task Signals ---\n");
}

void dump_uptime_summary(void) {
	console_write("\n--- System Uptime ---\n");
	u64 ticks = scheduler_get_ticks();
	console_write("  Uptime: ");
	console_write_dec(ticks / 100);
	console_write(".");
	u64 ms = (ticks % 100) * 10;
	if (ms < 100) console_write("0");
	if (ms < 10) console_write("0");
	console_write_dec(ms);
	console_write("s (");
	console_write_dec(ticks);
	console_write(" ticks, HZ=100)\n");
	console_write("--- End Uptime ---\n");
}


#undef panic

void panic(const char *message)
{
	panic_at(message, 0, 0);
}

void panic_at(const char *message, const char *file, int line)
{
	/* One panicking CPU owns the console; the rest stop without printing. */
	{
		static volatile int panic_owner; /* cpu_id + 1, 0 = unclaimed */
		struct percpu *pc = get_percpu();
		int me = (pc ? pc->cpu_id : 0) + 1;
		int unclaimed = 0;

		if (!__atomic_compare_exchange_n(&panic_owner, &unclaimed, me, 0,
		                                 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			if (panic_owner != me)
				arch_halt(); /* somebody else is printing; stay out of it */
		}
	}

	/* Own the console for the whole dump.
	 *
	 * Claiming panic_owner above only stops a SECOND panic from interleaving.
	 * A CPU that is still running normally goes on printing, and its output
	 * lands character by character inside the register dump -- which is how
	 * "sched: corrupt kernel stack pointer for pid ..." arrived shredded, with
	 * the pid and the two pointers, the whole point of the message, unreadable.
	 *
	 * Take the lock rather than busting it. Busting is only right when the
	 * holder is the CPU that just died, and that is what the bounded spin
	 * decides: whoever holds it has a few million cycles to finish a line, and
	 * if they do not they are gone and the lock is taken anyway. Held for the
	 * rest of this function, which never returns -- console_write sees
	 * console_lock_held_here() and prints straight through. */
	{
		u64 spins = 0;
		u64 cflags;

		while (!console_lock_try_acquire_irqsave(&cflags)) {
			if (++spins > 8000000ull) {
				console_bust_lock();
				console_lock_acquire_irqsave(&cflags);
				break;
			}
		}
	}

	/* If we are dying before the command line was parsed, the console is
	 * still holding every line of the boot back; release them, or the panic
	 * arrives without the context that explains it. */
	console_log_panic_flush();

	/* The marker every harness looks for, emitted before anything else.
	 *
	 * tests/smoke.sh ends an instance on "KERNEL PANIC" or "[PANIC]", CLAUDE.md
	 * documents both, and this function printed neither -- klog_write maps
	 * KLOG_PANIC to LOGLEVEL_EMERG, so a panic came out as "<0>message" and
	 * nothing matched. The string did exist, in kernel/lib/panic.c, which is
	 * not in the build; that dead copy is why everyone believed it was there.
	 *
	 * The cost was not cosmetic: a panicking instance stopped writing and the
	 * runner then waited out its full 320-second silence allowance before
	 * killing it. Three such lanes is most of the wall clock of a failing run.
	 */
	console_write("\nKERNEL PANIC: ");
	console_write(message ? message : "(no message)");
	serial_write("\nKERNEL PANIC: ");
	serial_write(message ? message : "(no message)");

	if (file && file[0]) {
		console_write(" at ");
		console_write(file);
		console_write(":");
		console_write_dec((u64)line);

		serial_write(" at ");
		serial_write(file);
	}
	console_write("\n");
	serial_write("\n");

	/* Print current CPU & Task state */
	struct percpu *pc = get_percpu();
	u32 cpu = pc ? pc->cpu_id : 0;
	console_write("  CPU: ");
	console_write_dec((u64)cpu);

	struct task *t = pc ? pc->cur_task : NULL;
	if (t) {
		console_write(" | Task: pid=");
		console_write_dec((u64)t->id);
		console_write(" ('");
		console_write(t->name ? t->name : "none");
		console_write("') state=");
		console_write(klog_task_state_str((int)t->state));

		if (t->wait_chan) {
			console_write(" wait_chan=0x");
			console_write_hex64((u64)(usize)t->wait_chan);
			ksym_print((u64)(usize)t->wait_chan);
		}

		console_write("\n  Stack: base=0x");
		console_write_hex64((u64)(usize)t->stack);
		console_write(" ksp=0x");
		console_write_hex64(t->kernel_stack_ptr);
	}
	console_write("\n");

	klog_write(KLOG_PANIC, message);

	/* Dump recent klog ring messages for crash context */
	klog_dump_recent(1024);

	/* Dump backtrace */
	panic_backtrace();

	/* Dump raw stack words with symbol lookup */
	dump_raw_stack_with_symbols((u64)(usize)__builtin_frame_address(0), 16);

	/* Dump task resources & signals if task available */
	if (t) {
		dump_task_signals(t);
		dump_task_fds(t);
		dump_task_vmas(t);
	}

	/* Dump recent syscall flight recorder */
	dump_recent_syscalls();

	/* Dump ftrace history if active */
	if (ftrace_count() > 0) {
		ftrace_dump();
	}

	/* Dump all active tasks in system */
	scheduler_dump_tasks();

	/* Dump mounted filesystems */
	vfs_dump_mounts();

	/* Dump SMP other CPUs state */
	dump_smp_cpus_state();

	/* Dump memory & resource summary */
	dump_memory_summary();

	/* Dump system uptime */
	dump_uptime_summary();

	/* Dump held locks if lockdep enabled */
	lockdep_dump_all();

	arch_halt();
}





