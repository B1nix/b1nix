/*
 * The vDSO's clock functions (built as a shared object, not linked into the
 * kernel).
 *
 * Every function here answers exactly what the corresponding system call would
 * have answered at that instant, by repeating the kernel's own arithmetic on
 * the parameters it publishes in the [vvar] page (see <b1nix/vdso.h>). What it
 * cannot answer the same way it does not answer at all: it makes the real
 * system call instead. That covers the CPU-time clocks, the two COARSE clocks
 * (tick-based in the kernel), unknown clock ids, a NULL result pointer (the
 * kernel reports EFAULT), and every clock while the page says the counter is
 * not the kernel's clock source.
 *
 * Constraints of the environment: position-independent with no relocations
 * (nothing would apply them), no libc, no floating point or SIMD state touched,
 * and nothing written anywhere but the caller's result buffers.
 */
#define B1NIX_VDSO_BUILD 1
#include <b1nix/vdso.h>

#if defined(__x86_64__)
#define VDSO_MODE             VDSO_CLOCK_X86_TSC
#define NR_CLOCK_GETTIME      228
#define NR_CLOCK_GETRES       229
#define NR_GETTIMEOFDAY       96
#define NR_TIME               201
#define VDSO_FN(name)         __vdso_##name
#elif defined(__aarch64__)
#define VDSO_MODE             VDSO_CLOCK_ARM64_CNTVCT
#define NR_CLOCK_GETTIME      113
#define NR_CLOCK_GETRES       114
#define NR_GETTIMEOFDAY       169
#define VDSO_FN(name)         __kernel_##name
#else
#error "no vDSO for this architecture"
#endif

/* Everything is hidden (-fvisibility=hidden) except what the version script
 * exports. */
#define VDSO_EXPORT __attribute__((visibility("default")))

#define CLOCK_REALTIME        0
#define CLOCK_MONOTONIC       1
#define CLOCK_MONOTONIC_RAW   4
#define CLOCK_BOOTTIME        7

#define NSEC_PER_SEC  1000000000ull
#define NSEC_PER_USEC 1000ull

struct vdso_timespec {
	i64 tv_sec;
	i64 tv_nsec;
};

struct vdso_timeval {
	i64 tv_sec;
	i64 tv_usec;
};

struct vdso_timezone {
	i32 tz_minuteswest;
	i32 tz_dsttime;
};

/* The data page, one page below this object's ELF header (vdso.lds.S).
 * Volatile, because the kernel rewrites it underneath this code: every field
 * read has to be a real load, taken between the two sequence-count reads, or
 * the compiler may keep a value from before a retry. */
typedef const volatile struct vdso_data vvar_t;
extern vvar_t vdso_vvar_page __attribute__((visibility("hidden")));

/* kernel/vdso/vdso_<arch>.S */
long __b1nix_vdso_syscall2(long nr, long a0, long a1) __attribute__((visibility("hidden")));
u64 __b1nix_vdso_counter(void) __attribute__((visibility("hidden")));

static int fast_clock(int clk)
{
	return clk == CLOCK_REALTIME || clk == CLOCK_MONOTONIC ||
	       clk == CLOCK_MONOTONIC_RAW || clk == CLOCK_BOOTTIME;
}

/* One consistent reading of `clk` in nanoseconds. 0 on success, -1 when the
 * page says this reading is the system call's to make.
 *
 * Each field is loaded once into a local, so a value torn by a concurrent
 * update can at worst produce a result the sequence check then discards —
 * never a division by a zero that was only half written. */
static int read_clock_ns(int clk, u64 *out)
{
	vvar_t *d = &vdso_vvar_page;

	for (;;) {
		u32 seq = d->seq;

		if (seq & 1u)
			continue; /* a writer is in the middle of an update */
		if (d->version != VDSO_DATA_VERSION || d->clock_mode != VDSO_MODE)
			return -1;

		u64 base = d->counter_base;
		u64 div = d->counter_div;
		u64 scale = d->counter_scale;

		if (!div) {
			if (d->seq != seq)
				continue;
			return -1;
		}

		/* arch_tsc_monotonic_ns() without its cross-CPU clamp. The page only
		 * offers the counter once the kernel has found that it does not run
		 * backwards between CPUs (x86_64: the boot-time warp check; aarch64:
		 * one architectural counter for the whole system), and with that the
		 * clamp never changes a value. */
		u64 c = __b1nix_vdso_counter() - base;
		u64 ns = (c / div) * scale + ((c % div) * scale) / div;

		if (clk == CLOCK_REALTIME) {
			/* rtc_now_unix_nanos -> wallclock_now_ns -> ktime_monotonic_ns */
			if (!d->ktime_active || !d->wall_ready) {
				if (d->seq != seq)
					continue;
				return -1;
			}
			u64 kbase = d->ktime_base_ns;
			u64 korigin = d->ktime_origin_ns;
			u64 kt = ns < korigin ? kbase : kbase + (ns - korigin);

			/* wallclock.c slew_applied(kt) */
			i64 slew = d->wall_slew_ns;
			u64 slew_mono = d->wall_slew_mono_ns;
			u64 slew_div = d->wall_slew_div;
			i64 applied = 0;

			if (slew && slew_div) {
				u64 limit = (kt - slew_mono) / slew_div;
				u64 mag = slew < 0 ? (u64)-slew : (u64)slew;

				if (limit > mag)
					limit = mag;
				applied = slew < 0 ? -(i64)limit : (i64)limit;
			}
			i64 wall = d->wall_base_ns + (i64)kt + applied;
			ns = wall < 0 ? 0 : (u64)wall;
		}

		if (d->seq == seq) {
			*out = ns;
			return 0;
		}
	}
}

VDSO_EXPORT int VDSO_FN(clock_gettime)(int clk, struct vdso_timespec *ts)
{
	u64 ns;

	if (ts && fast_clock(clk) && read_clock_ns(clk, &ns) == 0) {
		ts->tv_sec = (i64)(ns / NSEC_PER_SEC);
		ts->tv_nsec = (i64)(ns % NSEC_PER_SEC);
		return 0;
	}
	return (int)__b1nix_vdso_syscall2(NR_CLOCK_GETTIME, clk, (long)ts);
}

VDSO_EXPORT int VDSO_FN(gettimeofday)(struct vdso_timeval *tv, struct vdso_timezone *tz)
{
	u64 ns;

	if (!tv || read_clock_ns(CLOCK_REALTIME, &ns) != 0)
		return (int)__b1nix_vdso_syscall2(NR_GETTIMEOFDAY, (long)tv, (long)tz);
	tv->tv_sec = (i64)(ns / NSEC_PER_SEC);
	tv->tv_usec = (i64)((ns % NSEC_PER_SEC) / NSEC_PER_USEC);
	if (tz) {
		/* The kernel keeps UTC and no DST state: {0, 0}, as the call says. */
		tz->tz_minuteswest = 0;
		tz->tz_dsttime = 0;
	}
	return 0;
}

VDSO_EXPORT int VDSO_FN(clock_getres)(int clk, struct vdso_timespec *res)
{
	vvar_t *d = &vdso_vvar_page;

	/* With the counter as the clock source the kernel reports one nanosecond
	 * for these clocks (sys_clock_getres); anything else it answers itself. */
	if (!fast_clock(clk) || d->version != VDSO_DATA_VERSION ||
	    d->clock_mode != VDSO_MODE)
		return (int)__b1nix_vdso_syscall2(NR_CLOCK_GETRES, clk, (long)res);
	if (res) {
		res->tv_sec = 0;
		res->tv_nsec = 1;
	}
	return 0;
}

#if defined(__x86_64__)
VDSO_EXPORT i64 VDSO_FN(time)(i64 *t)
{
	u64 ns;

	if (read_clock_ns(CLOCK_REALTIME, &ns) != 0)
		return __b1nix_vdso_syscall2(NR_TIME, (long)t, 0);
	i64 secs = (i64)(ns / NSEC_PER_SEC);
	if (t)
		*t = secs;
	return secs;
}

/* The unprefixed names Linux's x86_64 vDSO also exports. */
VDSO_EXPORT int clock_gettime(int, struct vdso_timespec *)
	__attribute__((weak, alias("__vdso_clock_gettime")));
VDSO_EXPORT int gettimeofday(struct vdso_timeval *, struct vdso_timezone *)
	__attribute__((weak, alias("__vdso_gettimeofday")));
VDSO_EXPORT i64 time(i64 *) __attribute__((weak, alias("__vdso_time")));
VDSO_EXPORT int clock_getres(int, struct vdso_timespec *)
	__attribute__((weak, alias("__vdso_clock_getres")));
#endif
