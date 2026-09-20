/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fpu_ctx_smoke — vector register state across context switches.
 *
 * Every thread loads its 16 YMM registers with values only it knows, then
 * spends a while being preempted, yielding, sleeping and taking signals, and
 * checks the registers after each of those. A register that comes back with
 * another thread's value, or zeroed in its upper half, is FPU state the kernel
 * did not save or restore (XSAVE mask, area size, kernel code using SSE, a
 * signal frame without the extended state). A desktop toolkit computing
 * layout in AVX code shows that as sizes of 1e-8 and icons that vanish.
 *
 * Markers: FPU-CTX: ok ymm-across-yield, ok ymm-across-sleep,
 *          ok ymm-across-signal, ok xmm-across-yield; FAIL with a count.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#define THREADS 6
#if defined(__x86_64__)
#define WIDE_BYTES 512
#else
#define WIDE_BYTES 256
#endif
#define ROUNDS 4000

static volatile int fails_yield, fails_sleep, fails_signal, fails_xmm;
static volatile int have_avx;

/* Nothing but the kernel may run between a fill and its dump: libc is free to
 * use the vector registers (musl's aarch64 memset does), so the waits are raw
 * system calls and the buffers are cleared before the fill. */
#if defined(__x86_64__)
#define NR_gettid 186
#define NR_sched_yield 24
#define NR_nanosleep 35
#define NR_tgkill 234
static long raw_syscall(long nr, long a, long b, long c)
{
	long ret;
	__asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c)
	                 : "rcx", "r11", "memory");
	return ret;
}
#elif defined(__aarch64__)
#define NR_gettid 178
#define NR_sched_yield 124
#define NR_nanosleep 101
#define NR_tgkill 131
static long raw_syscall(long nr, long a, long b, long c)
{
	register long x8 __asm__("x8") = nr;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
	return x0;
}
#endif

static void sig_handler(int sig)
{
	(void)sig;
	/* Burn the vector registers in the handler, as any handler may (they are
	 * caller-saved): a kernel whose signal frame has no FPU image hands the
	 * interrupted code these values. */
	static const uint64_t junk[8] = {
		0xDEADBEEFDEADBEEFull, 0xDEADBEEFDEADBEEFull,
		0xDEADBEEFDEADBEEFull, 0xDEADBEEFDEADBEEFull,
		0xDEADBEEFDEADBEEFull, 0xDEADBEEFDEADBEEFull,
		0xDEADBEEFDEADBEEFull, 0xDEADBEEFDEADBEEFull,
	};
#if defined(__x86_64__)
	__asm__ volatile(
		"vmovdqu (%0), %%ymm0\n\t vmovdqu 32(%0), %%ymm1\n\t"
		"vmovdqu (%0), %%ymm7\n\t vmovdqu 32(%0), %%ymm15\n\t"
		: : "r"(junk) : "memory", "xmm0", "xmm1", "xmm7", "xmm15");
#elif defined(__aarch64__)
	__asm__ volatile(
		"ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%0]\n\t"
		"ld1 {v8.16b, v9.16b, v10.16b, v11.16b}, [%0]\n\t"
		"ld1 {v14.16b, v15.16b}, [%0]\n\t"
		: : "r"(junk) : "memory", "v0", "v1", "v2", "v3",
		"v8", "v9", "v10", "v11", "v14", "v15");
#endif
}

#if defined(__x86_64__)
/* Fill ymm0..ymm15 with a per-thread pattern and read them back. */
static void ymm_fill(const uint64_t *pat)
{
	__asm__ volatile(
		"vmovdqu (%0), %%ymm0\n\t vmovdqu 32(%0), %%ymm1\n\t"
		"vmovdqu 64(%0), %%ymm2\n\t vmovdqu 96(%0), %%ymm3\n\t"
		"vmovdqu 128(%0), %%ymm4\n\t vmovdqu 160(%0), %%ymm5\n\t"
		"vmovdqu 192(%0), %%ymm6\n\t vmovdqu 224(%0), %%ymm7\n\t"
		"vmovdqu 256(%0), %%ymm8\n\t vmovdqu 288(%0), %%ymm9\n\t"
		"vmovdqu 320(%0), %%ymm10\n\t vmovdqu 352(%0), %%ymm11\n\t"
		"vmovdqu 384(%0), %%ymm12\n\t vmovdqu 416(%0), %%ymm13\n\t"
		"vmovdqu 448(%0), %%ymm14\n\t vmovdqu 480(%0), %%ymm15\n\t"
		: : "r"(pat) : "memory",
		"xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7",
		"xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15");
}

static void ymm_dump(uint64_t *out)
{
	__asm__ volatile(
		"vmovdqu %%ymm0, (%0)\n\t vmovdqu %%ymm1, 32(%0)\n\t"
		"vmovdqu %%ymm2, 64(%0)\n\t vmovdqu %%ymm3, 96(%0)\n\t"
		"vmovdqu %%ymm4, 128(%0)\n\t vmovdqu %%ymm5, 160(%0)\n\t"
		"vmovdqu %%ymm6, 192(%0)\n\t vmovdqu %%ymm7, 224(%0)\n\t"
		"vmovdqu %%ymm8, 256(%0)\n\t vmovdqu %%ymm9, 288(%0)\n\t"
		"vmovdqu %%ymm10, 320(%0)\n\t vmovdqu %%ymm11, 352(%0)\n\t"
		"vmovdqu %%ymm12, 384(%0)\n\t vmovdqu %%ymm13, 416(%0)\n\t"
		"vmovdqu %%ymm14, 448(%0)\n\t vmovdqu %%ymm15, 480(%0)\n\t"
		: : "r"(out) : "memory");
}

static void xmm_fill(const uint64_t *pat)
{
	__asm__ volatile(
		"movdqu (%0), %%xmm0\n\t movdqu 16(%0), %%xmm1\n\t"
		"movdqu 32(%0), %%xmm2\n\t movdqu 48(%0), %%xmm3\n\t"
		"movdqu 64(%0), %%xmm4\n\t movdqu 80(%0), %%xmm5\n\t"
		"movdqu 96(%0), %%xmm6\n\t movdqu 112(%0), %%xmm7\n\t"
		"movdqu 128(%0), %%xmm8\n\t movdqu 144(%0), %%xmm9\n\t"
		"movdqu 160(%0), %%xmm10\n\t movdqu 176(%0), %%xmm11\n\t"
		"movdqu 192(%0), %%xmm12\n\t movdqu 208(%0), %%xmm13\n\t"
		"movdqu 224(%0), %%xmm14\n\t movdqu 240(%0), %%xmm15\n\t"
		: : "r"(pat) : "memory",
		"xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7",
		"xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15");
}

static void xmm_dump(uint64_t *out)
{
	__asm__ volatile(
		"movdqu %%xmm0, (%0)\n\t movdqu %%xmm1, 16(%0)\n\t"
		"movdqu %%xmm2, 32(%0)\n\t movdqu %%xmm3, 48(%0)\n\t"
		"movdqu %%xmm4, 64(%0)\n\t movdqu %%xmm5, 80(%0)\n\t"
		"movdqu %%xmm6, 96(%0)\n\t movdqu %%xmm7, 112(%0)\n\t"
		"movdqu %%xmm8, 128(%0)\n\t movdqu %%xmm9, 144(%0)\n\t"
		"movdqu %%xmm10, 160(%0)\n\t movdqu %%xmm11, 176(%0)\n\t"
		"movdqu %%xmm12, 192(%0)\n\t movdqu %%xmm13, 208(%0)\n\t"
		"movdqu %%xmm14, 224(%0)\n\t movdqu %%xmm15, 240(%0)\n\t"
		: : "r"(out) : "memory");
}

static int cpu_has_avx(void)
{
	unsigned a, b, c, d;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
	if (!(c & (1u << 28)) || !(c & (1u << 27)))
		return 0;
	unsigned lo, hi;
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	return (lo & 6) == 6;
}

#elif defined(__aarch64__)
/* aarch64: v0..v15 (the NEON/FP registers) carry the pattern; the kernel's
 * signal frame must hold the whole fpsimd state and its context switch too.
 * "avx" reads as "the wide half" here: one register file, so ymm_* fills
 * v0..v15 with 256 bytes of pattern and xmm_* the same. */
static void ymm_fill(const uint64_t *pat)
{
	__asm__ volatile(
		"ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%0], #64\n\t"
		"ld1 {v4.16b, v5.16b, v6.16b, v7.16b}, [%0], #64\n\t"
		"ld1 {v8.16b, v9.16b, v10.16b, v11.16b}, [%0], #64\n\t"
		"ld1 {v12.16b, v13.16b, v14.16b, v15.16b}, [%0], #64\n\t"
		: "+r"(pat) : : "memory",
		"v0","v1","v2","v3","v4","v5","v6","v7",
		"v8","v9","v10","v11","v12","v13","v14","v15");
}

static void ymm_dump(uint64_t *out)
{
	__asm__ volatile(
		"st1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%0], #64\n\t"
		"st1 {v4.16b, v5.16b, v6.16b, v7.16b}, [%0], #64\n\t"
		"st1 {v8.16b, v9.16b, v10.16b, v11.16b}, [%0], #64\n\t"
		"st1 {v12.16b, v13.16b, v14.16b, v15.16b}, [%0], #64\n\t"
		: "+r"(out) : : "memory");
}

static void xmm_fill(const uint64_t *pat) { ymm_fill(pat); }
static void xmm_dump(uint64_t *out) { ymm_dump(out); }

static int cpu_has_avx(void) { return 1; }

#else
#error unsupported architecture
#endif

static void *worker(void *arg)
{
	uint64_t id = (uint64_t)(uintptr_t)arg;
	uint64_t pat[64], got[64];
	for (int i = 0; i < 64; i++)
		pat[i] = 0x1111111111111111ull * (id + 1) + (uint64_t)i * 0x0101010101010101ull;

	long pid = getpid();
	long tid = raw_syscall(NR_gettid, 0, 0, 0);
	struct timespec nap = { 0, 200000 };

	for (int r = 0; r < ROUNDS; r++) {
		int mode = r % 3;
		if (have_avx) {
			memset(got, 0, sizeof(got));
			ymm_fill(pat);
			if (mode == 0)
				raw_syscall(NR_sched_yield, 0, 0, 0);
			else if (mode == 1)
				raw_syscall(NR_nanosleep, (long)(uintptr_t)&nap, 0, 0);
			else
				raw_syscall(NR_tgkill, pid, tid, SIGUSR1);
			ymm_dump(got);
			if (memcmp(pat, got, WIDE_BYTES)) {
				if (mode == 0) __sync_fetch_and_add(&fails_yield, 1);
				else if (mode == 1) __sync_fetch_and_add(&fails_sleep, 1);
				else __sync_fetch_and_add(&fails_signal, 1);
			}
		}
		memset(got, 0, sizeof(got));
		xmm_fill(pat);
		raw_syscall(NR_sched_yield, 0, 0, 0);
		xmm_dump(got);
		if (memcmp(pat, got, 256))
			__sync_fetch_and_add(&fails_xmm, 1);
	}
	return 0;
}

int main(void)
{
	pthread_t th[THREADS];
	have_avx = cpu_has_avx();
	signal(SIGUSR1, sig_handler);
	printf("FPU-CTX: avx %s, %d threads x %d rounds\n", have_avx ? "yes" : "no", THREADS, ROUNDS);
	for (uint64_t i = 0; i < THREADS; i++)
		pthread_create(&th[i], 0, worker, (void *)(uintptr_t)i);
	for (int i = 0; i < THREADS; i++)
		pthread_join(th[i], 0);
	int bad = 0;
#define REPORT(name, v, skip) do { \
	if (skip) printf("FPU-CTX: skip %s (no avx)\n", name); \
	else if (v) { printf("FPU-CTX: FAIL %s (%d corruptions)\n", name, v); bad = 1; } \
	else printf("FPU-CTX: ok %s\n", name); } while (0)
#if defined(__aarch64__)
	/* One register file on this arch: "ymm" is v0..v15, "avx" always there. */
#endif
	REPORT("ymm-across-yield", fails_yield, !have_avx);
	REPORT("ymm-across-sleep", fails_sleep, !have_avx);
	REPORT("ymm-across-signal", fails_signal, !have_avx);
	REPORT("xmm-across-yield", fails_xmm, 0);
	printf("FPU-CTX: done (%d failed)\n", bad);
	return bad;
}
