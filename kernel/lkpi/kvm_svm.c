// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD SVM glue (M131).
 *
 * The SVM entry code (svm/vmenter.S) pushes this CPU's host save-area address
 * from a %gs-relative per-CPU variable, and both entry paths compare the
 * host's SPEC_CTRL the same way. Those are mirrored into b1nix's struct
 * percpu (<b1nix/percpu_kvm.h>) once the vendor module has set them up: the
 * save areas are allocated per CPU in svm_hardware_setup and never move.
 */
#include <linux/kvm_host.h>
#include <asm/nospec-branch.h>
#include "svm/svm.h"
#include <b1nix/kvm_bridge.h>

void kvm_percpu_mirror_all(void);

void kvm_percpu_mirror_all(void)
{
	for (int cpu = 0; cpu < b1nix_kvm_cpu_count(); cpu++) {
		struct svm_cpu_data *sd = per_cpu_ptr(&svm_data, cpu);

		b1nix_kvm_percpu_mirror(cpu, per_cpu(x86_spec_ctrl_current, cpu),
					sd ? sd->save_area_pa : 0);
	}
}
