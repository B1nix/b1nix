/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_MSR_H
#define KVM_SHIM_ASM_MSR_H
#ifndef __ASSEMBLY__
/* MSR access as KVM calls it (M131). The *_safe forms catch the #GP a missing
 * MSR raises (lkpi's extable, kernel/lkpi/kvm_x86_cpu.c) and return -EIO. */
#include <linux/types.h>
#include <asm/msr-index.h>

struct msr {
	union {
		struct {
			u32 l;
			u32 h;
		};
		u64 q;
	};
};

struct msr_info {
	u32 msr_no;
	struct msr reg;
	struct msr *msrs;
	int err;
};

static inline u64 native_rdmsrq(u32 msr)
{
	u32 lo, hi;

	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((u64)hi << 32) | lo;
}

static inline void native_wrmsrq(u32 msr, u64 val)
{
	__asm__ volatile("wrmsr" : : "c"(msr), "a"((u32)val), "d"((u32)(val >> 32))
			 : "memory");
}

#define native_rdmsr(msr, low, high)                                     \
	do {                                                             \
		u64 __v = native_rdmsrq(msr);                            \
		(void)((low) = (u32)__v);                                \
		(void)((high) = (u32)(__v >> 32));                       \
	} while (0)
#define native_wrmsr(msr, low, high) \
	native_wrmsrq(msr, ((u64)(u32)(high) << 32) | (u32)(low))

/* Faulting reads and writes: 0, or -EIO when the MSR does not exist. */
int lkpi_rdmsrq_safe(u32 msr, u64 *val);
int lkpi_wrmsrq_safe(u32 msr, u64 val);

#define rdmsrq(msr, val)      ((val) = native_rdmsrq(msr))
#define wrmsrq(msr, val)      native_wrmsrq((msr), (val))
#define rdmsrl(msr, val)      rdmsrq(msr, val)
#define wrmsrl(msr, val)      wrmsrq(msr, val)
#define rdmsr(msr, low, high) native_rdmsr(msr, low, high)
#define wrmsr(msr, low, high) native_wrmsr(msr, low, high)
#define wrmsrns(msr, val)     native_wrmsrq((msr), (val))
#define rdmsrq_safe(msr, p)   lkpi_rdmsrq_safe((msr), (p))
#define wrmsrq_safe(msr, val) lkpi_wrmsrq_safe((msr), (val))
#define native_read_msr(msr)  native_rdmsrq(msr)
#define native_write_msr(msr, val) native_wrmsrq((msr), (val))
#define native_read_msr_safe(msr, p)       lkpi_rdmsrq_safe((msr), (p))
#define native_write_msr_safe(msr, val)    lkpi_wrmsrq_safe((msr), (val))

/* Set one bit of an MSR: 1 when it changed, 0 when it was already set, a
 * negative errno when the MSR is not there -- as upstream. */
static inline int msr_set_bit(u32 msr, u8 bit)
{
	u64 v;

	if (lkpi_rdmsrq_safe(msr, &v))
		return -EIO;
	if (v & (1ULL << bit))
		return 0;
	if (lkpi_wrmsrq_safe(msr, v | (1ULL << bit)))
		return -EIO;
	return 1;
}

static inline int rdmsr_safe(u32 msr, u32 *lo, u32 *hi)
{
	u64 v = 0;
	int err = lkpi_rdmsrq_safe(msr, &v);

	*lo = (u32)v;
	*hi = (u32)(v >> 32);
	return err;
}

static inline int wrmsr_safe(u32 msr, u32 lo, u32 hi)
{
	return lkpi_wrmsrq_safe(msr, ((u64)hi << 32) | lo);
}

int rdmsrq_on_cpu(unsigned int cpu, u32 msr, u64 *val);
int wrmsrq_on_cpu(unsigned int cpu, u32 msr, u64 val);

static inline u64 rdtsc(void)
{
	u32 lo, hi;

	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((u64)hi << 32) | lo;
}

static inline u64 rdtsc_ordered(void)
{
	u32 lo, hi;

	__asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
	return ((u64)hi << 32) | lo;
}

#define rdtscp(aux)                                                      \
	({                                                               \
		u32 __lo, __hi, __aux;                                   \
		__asm__ volatile("rdtscp" : "=a"(__lo), "=d"(__hi), "=c"(__aux)); \
		(aux) = __aux;                                           \
		((u64)__hi << 32) | __lo;                                \
	})
#define rdpid(val) ({ u64 __t = 0; (void)rdtscp(val); __t; })
#endif
#endif
