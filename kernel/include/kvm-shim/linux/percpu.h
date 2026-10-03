/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_LINUX_PERCPU_H
#define KVM_SHIM_LINUX_PERCPU_H
/*
 * Real per-CPU variables for KVM (M131).
 *
 * lkpi's <linux/percpu.h> gives every "per-CPU" variable one instance, which
 * is right for the DRM and filesystem code that only uses them as caches. KVM
 * cannot live with that: the VMXON region, the VMCS each CPU has loaded and
 * the host MSR values it must restore are all per processor, and one shared
 * copy would have two CPUs executing VMXON on the same page.
 *
 * So KVM's are Linux's model in miniature. A DEFINE_PER_CPU variable is laid
 * out in the section `kvm_percpu`, which is only a template; lkpi gives each
 * CPU its own copy of the section at init (kvm_percpu_init) and a per-CPU
 * pointer is translated by its offset into the template. alloc_percpu carves
 * from an arena inside the same template, so static and dynamic per-CPU
 * pointers translate the same way.
 */
#include <linux/types.h>
#include <linux/smp.h>
#include <linux/preempt.h>
#include <linux/slab.h>

extern char __start_kvm_percpu[], __stop_kvm_percpu[];
extern char *kvm_percpu_base[];   /* per CPU: its copy of the section */

#define __percpu
#define __PCPU_ATTRS __attribute__((section("kvm_percpu"), aligned(64)))

#define DECLARE_PER_CPU(type, name) extern __typeof__(type) name
#define DEFINE_PER_CPU(type, name) __PCPU_ATTRS __typeof__(type) name
#define DEFINE_PER_CPU_ALIGNED(type, name) DEFINE_PER_CPU(type, name)
#define DEFINE_PER_CPU_SHARED_ALIGNED(type, name) DEFINE_PER_CPU(type, name)
#define DEFINE_PER_CPU_READ_MOSTLY(type, name) DEFINE_PER_CPU(type, name)
#define DECLARE_PER_CPU_ALIGNED(type, name) DECLARE_PER_CPU(type, name)
#define DECLARE_PER_CPU_READ_MOSTLY(type, name) DECLARE_PER_CPU(type, name)
#define EXPORT_PER_CPU_SYMBOL(var)
#define EXPORT_PER_CPU_SYMBOL_GPL(var)

#define per_cpu_ptr(ptr, cpu)                                            \
	((__typeof__(ptr))(kvm_percpu_base[(cpu)] +                      \
			   ((char *)(ptr) - __start_kvm_percpu)))
#define raw_cpu_ptr(ptr)   per_cpu_ptr(ptr, raw_smp_processor_id())
#define this_cpu_ptr(ptr)  per_cpu_ptr(ptr, smp_processor_id())
#define per_cpu(var, cpu)  (*per_cpu_ptr(&(var), cpu))
#define get_cpu_ptr(ptr)   ({ preempt_disable(); this_cpu_ptr(ptr); })
#define put_cpu_ptr(ptr)   do { (void)(ptr); preempt_enable(); } while (0)
#define get_cpu_var(var)   (*get_cpu_ptr(&(var)))
#define put_cpu_var(var)   put_cpu_ptr(&(var))

#define this_cpu_read(var)        (*this_cpu_ptr(&(var)))
#define this_cpu_write(var, v)    ((void)(*this_cpu_ptr(&(var)) = (v)))
#define this_cpu_add(var, n)      ((void)(*this_cpu_ptr(&(var)) += (n)))
#define this_cpu_inc(var)         this_cpu_add(var, 1)
#define this_cpu_dec(var)         this_cpu_add(var, -1)
#define __this_cpu_read(var)      this_cpu_read(var)
#define __this_cpu_write(var, v)  this_cpu_write(var, v)
#define __this_cpu_add(var, n)    this_cpu_add(var, n)
#define __this_cpu_inc(var)       this_cpu_inc(var)
#define raw_cpu_read(var)         this_cpu_read(var)
#define raw_cpu_write(var, v)     this_cpu_write(var, v)
#define this_cpu_xchg(var, v)     ({ __typeof__(var) __o = this_cpu_read(var); this_cpu_write(var, v); __o; })
#define this_cpu_cmpxchg(var, o, n) \
	({ __typeof__(var) __c = this_cpu_read(var); if (__c == (o)) this_cpu_write(var, n); __c; })

/* Dynamic per-CPU memory, from the template's arena; NULL when it is full. */
void __percpu *__alloc_percpu(size_t size, size_t align);
void free_percpu(void __percpu *p);
#define alloc_percpu(type) \
	((__typeof__(type) __percpu *)__alloc_percpu(sizeof(type), __alignof__(type)))
#define alloc_percpu_gfp(type, gfp) ({ (void)(gfp); alloc_percpu(type); })

/* Give every CPU its copy of the template; 0 or -ENOMEM. */
int kvm_percpu_init(void);
#endif
