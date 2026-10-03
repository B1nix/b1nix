// SPDX-License-Identifier: GPL-2.0-only
/*
 * Early boot parameters (early_param, <linux/init.h>) for KVM (M131): arm64
 * KVM reads kvm-arm.mode and its trap policies this way. Each registration
 * is a record in the lkpi_early_param section; this walks the kernel
 * command line once and hands every name=value (or bare name, as "") whose
 * name is registered to its function, before KVM initialises.
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/string.h>
#include <b1nix/kvm_bridge.h>

/* The linker's bounds of the section; weak, because a kernel whose KVM
 * registers no early parameter has no such section. */
extern const struct lkpi_early_param __start_lkpi_early_param[] __attribute__((weak));
extern const struct lkpi_early_param __stop_lkpi_early_param[] __attribute__((weak));

#define PARAM_MAX 256

void lkpi_run_early_params(void)
{
	const char *p = b1nix_kvm_cmdline();

	while (*p) {
		char word[PARAM_MAX];
		size_t n = 0;
		char *val;

		while (*p == ' ')
			p++;
		while (*p && *p != ' ') {
			if (n < sizeof(word) - 1)
				word[n++] = *p;
			p++;
		}
		word[n] = 0;
		if (!n)
			continue;
		val = strchr(word, '=');
		if (val)
			*val++ = 0;
		for (const struct lkpi_early_param *e = __start_lkpi_early_param;
		     e < __stop_lkpi_early_param; e++) {
			if (strcmp(e->name, word))
				continue;
			if (e->fn(val ? val : (char *)"") < 0)
				pr_warn("kvm: bad value for %s\n", word);
		}
	}
}
