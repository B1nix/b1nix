/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_ASM_BARRIER_H
#define LKPI_ASM_BARRIER_H

#include <linux/compiler.h>

/*
 * Memory barriers, x86_64.
 *
 * x86 is TSO: loads are not reordered with loads, stores are not reordered with
 * stores, and the only reordering the hardware performs is a load moving ahead
 * of an earlier store. So `smp_rmb` and `smp_wmb` need nothing from the CPU and
 * everything from the compiler, while `smp_mb` needs the fence.
 *
 * `lock addl $0,-4(%rsp)` rather than `mfence`: it is the sequence upstream
 * settled on for the same reason, being cheaper on every microarchitecture that
 * matters and equally ordering. The negative offset stays below the stack
 * pointer, which the red zone makes safe to touch — and the kernel is built
 * -mno-red-zone precisely so nothing else is using it.
 */

/*
 * Guarded one by one rather than as a block. <linux/smp.h> and <linux/atomic.h>
 * in this tree already define some of these — they predate this header, having
 * been written for the DRM import — and a second definition of a macro is an
 * error, not a merge. The guards let whichever header is reached first win,
 * and the definitions agree.
 */
/*
 * The full fence differs by architecture and this tree builds for two.
 *
 * x86-64 is TSO, so the only reordering to stop is a load moving ahead of an
 * earlier store, and `lock addl $0,-4(%rsp)` is the sequence upstream settled
 * on for it — cheaper than `mfence` on every microarchitecture that matters and
 * equally ordering. The negative offset stays below the stack pointer, which is
 * safe to touch because the kernel is built -mno-red-zone and nothing else is
 * using it.
 *
 * aarch64 is weakly ordered and needs a real barrier for all three: `dmb ish`,
 * inner-shareable, which is the domain the CPUs in one machine share.
 */
/*
 * Upstream's, instruction for instruction (arch/x86 and arch/arm64 barrier.h).
 *
 * mb/rmb/wmb order against devices as well as other CPUs: on x86 that is
 * mfence/lfence/sfence -- write-combining stores, which a GPU ring written
 * through a WC mapping is made of, are not ordered by the CPU's ordinary rules,
 * and a doorbell written after them without sfence can reach the device first.
 * These were compiler barriers here, which is the smp_ variant. On arm64 the
 * device forms are dsb, full system; the dma_ forms are dmb over the outer
 * shareable domain, which is where a device sits; only smp_ is inner.
 */
#ifndef mb
#if defined(__x86_64__)
#define mb()  __asm__ __volatile__("mfence" ::: "memory")
#elif defined(__aarch64__)
#define mb()  __asm__ __volatile__("dsb sy" ::: "memory")
#else
#define mb()  __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif
#endif
#ifndef rmb
#if defined(__x86_64__)
#define rmb() __asm__ __volatile__("lfence" ::: "memory")
#else
#define rmb() __asm__ __volatile__("dsb ld" ::: "memory")
#endif
#endif
#ifndef wmb
#if defined(__x86_64__)
#define wmb() __asm__ __volatile__("sfence" ::: "memory")
#else
#define wmb() __asm__ __volatile__("dsb st" ::: "memory")
#endif
#endif

#if defined(__x86_64__)
#ifndef smp_mb
#define smp_mb()  __asm__ __volatile__("lock; addl $0,-4(%%rsp)" ::: "memory", "cc")
#endif
/* x86 never reorders loads with loads or stores with stores between CPUs. */
#ifndef smp_rmb
#define smp_rmb() barrier()
#endif
#ifndef smp_wmb
#define smp_wmb() barrier()
#endif
#else
#ifndef smp_mb
#define smp_mb()  __asm__ __volatile__("dmb ish" ::: "memory")
#endif
#ifndef smp_rmb
#define smp_rmb() __asm__ __volatile__("dmb ishld" ::: "memory")
#endif
#ifndef smp_wmb
#define smp_wmb() __asm__ __volatile__("dmb ishst" ::: "memory")
#endif
#endif

/*
 * Acquire/release. Expressed through the compiler's atomic builtins rather than
 * a barrier plus a plain access: on aarch64 the ordering is a property of the
 * instruction (ldar/stlr), and a fence bolted onto an ordinary load is both
 * slower and, for the release case, not the same thing.
 */
#ifndef smp_load_acquire
#define smp_load_acquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#endif

#ifndef smp_store_release
#define smp_store_release(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#endif

/* A dependent load needs no barrier on either architecture here — Alpha is the
 * only one that ever did — so the name exists to let imported code document the
 * dependency, and expands to nothing. */
#define smp_read_barrier_depends() do { } while (0)
#define read_barrier_depends()     do { } while (0)

#if defined(__x86_64__)
/* Coherent DMA memory is ordered like any other on x86. */
#define dma_rmb() barrier()
#define dma_wmb() barrier()
#else
#define dma_rmb() __asm__ __volatile__("dmb oshld" ::: "memory")
#define dma_wmb() __asm__ __volatile__("dmb oshst" ::: "memory")
#endif

#ifndef smp_mb__before_atomic
#define smp_mb__before_atomic() smp_mb()
#endif
#ifndef smp_mb__after_atomic
#define smp_mb__after_atomic()  smp_mb()
#endif

#define array_index_mask_nospec(index, size)                                   \
	((~0UL) + ((index) < (size)))

#endif
