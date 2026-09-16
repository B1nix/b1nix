/*
 * The vDSO: clock readings a process takes without entering the kernel.
 *
 * Two mappings are placed into every process at exec. [vvar] is one page the
 * kernel writes and userspace only reads; it holds, under a sequence count,
 * exactly the parameters the clock system calls compute their answers from.
 * [vdso] is a small shared object (kernel/vdso/) that reads the hardware
 * counter itself and repeats the kernel's arithmetic on those parameters, so
 * that clock_gettime(CLOCK_MONOTONIC) is a function call rather than a trap.
 *
 * The page sits immediately below the vDSO's ELF header, which is how the
 * position-independent code finds it: a fixed negative offset from its own
 * text, with no relocation to apply.
 *
 * This header is shared with the vDSO build, so it may use only <b1nix/types.h>.
 */
#ifndef B1NIX_VDSO_H
#define B1NIX_VDSO_H

#include <b1nix/types.h>

/* What the vDSO may use to answer. Anything other than the counter its own
 * architecture reads means "make the system call": the kernel sets 0 whenever
 * its own clock path would not read the counter (no invariant TSC, a counter
 * that was seen to run backwards between CPUs, no counter frequency). */
#define VDSO_CLOCK_SYSCALL       0u
#define VDSO_CLOCK_X86_TSC       1u
#define VDSO_CLOCK_ARM64_CNTVCT  2u

/* Layout version, first word after the sequence count. Bumped whenever the
 * structure changes, so a stale vDSO image fails safe (falls back to the
 * system call) instead of reading fields at the wrong offsets. */
#define VDSO_DATA_VERSION 1u

struct vdso_data {
	/* Odd while a writer is updating the page. A reader retries until it saw
	 * the same even value before and after reading. */
	u32 seq;
	u32 version;
	u32 clock_mode;
	u32 pad0;

	/* Counter -> nanoseconds since the counter's zero, the formula the
	 * architecture's arch_tsc_monotonic_ns() uses:
	 *   c  = counter - counter_base
	 *   ns = (c / counter_div) * counter_scale
	 *        + ((c % counter_div) * counter_scale) / counter_div
	 * x86_64: counter_div is the TSC rate in kHz and counter_scale 10^6.
	 * aarch64: counter_div is CNTFRQ_EL0 in Hz and counter_scale 10^9. */
	u64 counter_base;
	u64 counter_div;
	u64 counter_scale;

	/* kernel/lib/ktime.c: ktime = ktime_base_ns + (counter_ns - ktime_origin_ns),
	 * or ktime_base_ns while counter_ns is still below the origin. Until
	 * ktime_active is set the kernel's clock is the tick, which only the
	 * system call can read. */
	u32 ktime_active;
	/* kernel/lib/wallclock.c: set once the wall clock has a base. */
	u32 wall_ready;
	u64 ktime_base_ns;
	u64 ktime_origin_ns;

	/* REALTIME = wall_base_ns + ktime + slew_applied(ktime), where
	 * slew_applied grows by one nanosecond per wall_slew_div nanoseconds of
	 * ktime since wall_slew_mono_ns, up to |wall_slew_ns|. */
	i64 wall_base_ns;
	i64 wall_slew_ns;
	u64 wall_slew_mono_ns;
	u64 wall_slew_div;
};

#ifndef B1NIX_VDSO_BUILD
struct task;
struct user_loaded_image;

/* Allocate the data page and the vDSO text frames. Called once at boot, after
 * the physical allocator and the direct map exist and before any process. */
void vdso_init(void);

/* Update the data page. The writer changes fields between begin and end; the
 * pair serialises writers and brackets the change with the sequence count.
 * Returns the structure to write, which is a boot-time holding copy until
 * vdso_init has run (the page then starts from its contents). */
struct vdso_data *vdso_write_begin(u64 *flags);
void vdso_write_end(u64 flags);

/* Where the vDSO goes in a new image, or 0 when there is none to map. Chosen
 * before the initial stack is built, because AT_SYSINFO_EHDR names it. */
u64 vdso_choose_base(void);

/* Map [vvar] and [vdso] into the current address space at image->vdso_base.
 * 0 on success (or when there is nothing to map), -errno otherwise. */
int vdso_map_current(struct user_loaded_image *image);

/* Bytes of the vDSO mapping (a whole number of pages), 0 when absent. */
u64 vdso_text_size(void);

/* 1 if `frame` is one of the kernel's own vDSO frames. A debugger's poke must
 * not write through to them: every process on the machine maps the same ones. */
int vdso_frame_is_shared(u64 frame);
#endif

/* Values of vm_area.special for the two mappings. */
#define VMA_SPECIAL_NONE 0u
#define VMA_SPECIAL_VVAR 1u
#define VMA_SPECIAL_VDSO 2u

#endif
