/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_PERCPU_KVM_H
#define B1NIX_PERCPU_KVM_H
/*
 * Where in struct percpu (what GS points at) KVM's entry code finds the two
 * per-CPU values it reads with %gs-relative operands (M131). Upstream's
 * per-CPU area is GS-relative as a whole; here KVM's other per-CPU variables
 * are reached through their own table, and these two are mirrored into the
 * structure GS already names. Plain numbers so assembly can include them;
 * lapic.h checks they match the structure.
 */
#define PERCPU_KVM_SPEC_CTRL    0xff0  /* the host's SPEC_CTRL value */
#define PERCPU_KVM_SVM_HSAVE_PA 0xff8  /* this CPU's SVM host save area */
#endif
