/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_MSHYPERV_H
#define KVM_SHIM_ASM_MSHYPERV_H
#include <hyperv/hvhdk.h>
/* Never a Hyper-V guest: every enlightenment KVM would use to run faster on
 * one is off (CONFIG_HYPERV is not set). */
#define hv_is_hyperv_initialized() 0
/* Read only behind kvm_is_using_evmcs(), which is false without Hyper-V. */
struct ms_hyperv_info {
	u32 features;
	u32 misc_features;
	u32 hints;
	u32 nested_features;
};
extern struct ms_hyperv_info ms_hyperv;
static inline void set_hv_tscchange_cb(void (*cb)(void)) { (void)cb; }
static inline void clear_hv_tscchange_cb(void) { }
static inline void hyperv_stop_tsc_emulation(void) { }
struct ms_hyperv_tsc_page;
static inline struct ms_hyperv_tsc_page *hv_get_tsc_page(void) { return NULL; }
static inline u64 hv_read_tsc_page_tsc(const struct ms_hyperv_tsc_page *p, u64 *cur_tsc,
				       u64 *time)
{
	(void)p; (void)cur_tsc; (void)time;
	return 0;
}
static inline void *hv_get_vp_assist_page(unsigned int cpu) { (void)cpu; return NULL; }
#endif
