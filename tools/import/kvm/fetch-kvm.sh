#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Stage Linux's KVM for M131: the x86 core, VMX, and the generic virt/kvm, with
# the headers that belong to KVM itself. Everything else KVM stands on -- the
# scheduler, memory management, the x86 CPU plumbing -- comes from linuxkpi
# (kernel/include, kernel/lkpi), never from Linux's own headers.
#
# Same rules as the DRM and filesystem imports: the pinned release, the checksum
# verified before extraction, and imported source never edited -- a patch here
# would be a bug in the shim.

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../../.." && pwd)"

LINUX_VERSION="${LINUX_VERSION:-6.18.51}"
case "$LINUX_VERSION" in
6.18.51) LINUX_SHA256="ba2f60f858bf4d1f929101faa356c93dc8b925b17aaa9f95eabd4627758df613" ;;
*) echo "fetch-kvm: no pinned SHA256 for linux-$LINUX_VERSION" >&2; exit 1 ;;
esac
TARBALL="linux-${LINUX_VERSION}.tar.xz"
URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/${TARBALL}"

SRC_PARENT="$ROOT_DIR/build/src/linux"
STAGE_DIR="$ROOT_DIR/build/src/kvm-${LINUX_VERSION}"

mkdir -p "$SRC_PARENT"

if [ -d "$STAGE_DIR/virt" ]; then
	echo "$STAGE_DIR"
	exit 0
fi

TAR_PATH="$SRC_PARENT/$TARBALL"
if [ ! -f "$TAR_PATH" ]; then
	echo "fetch-kvm: downloading $TARBALL" >&2
	curl -L "$URL" -o "$TAR_PATH.part" 1>&2
	mv "$TAR_PATH.part" "$TAR_PATH"
fi

have="$(sha256sum "$TAR_PATH" | cut -d' ' -f1)"
if [ "$have" != "$LINUX_SHA256" ]; then
	echo "fetch-kvm: SHA256 mismatch for $TARBALL" >&2
	echo "  expected $LINUX_SHA256" >&2
	echo "  got      $have" >&2
	exit 1
fi

P="linux-${LINUX_VERSION}"
echo "fetch-kvm: staging KVM from $P" >&2
rm -rf "$STAGE_DIR.tmp"
mkdir -p "$STAGE_DIR.tmp"
# The sources, and the headers that are KVM's own: the generic kvm_host.h
# family, include/kvm, the uapi, and x86's kvm_host.h with the VMX/SVM
# architectural definitions it is written against.
tar -xf "$TAR_PATH" -C "$STAGE_DIR.tmp" --strip-components=1 \
	"$P/arch/x86/kvm" \
	"$P/virt/kvm" \
	"$P/include/kvm" \
	"$P/include/linux/kvm_host.h" \
	"$P/include/linux/kvm_types.h" \
	"$P/include/linux/kvm_para.h" \
	"$P/include/linux/kvm_irqfd.h" \
	"$P/include/linux/kvm_dirty_ring.h" \
	"$P/include/uapi/linux/kvm.h" \
	"$P/include/uapi/linux/kvm_para.h" \
	"$P/arch/x86/include/asm/kvm_host.h" \
	"$P/arch/x86/include/asm/kvm_types.h" \
	"$P/arch/x86/include/asm/kvm_para.h" \
	"$P/arch/x86/include/asm/kvm_page_track.h" \
	"$P/arch/x86/include/asm/kvm_vcpu_regs.h" \
	"$P/arch/x86/include/asm/kvm-x86-ops.h" \
	"$P/arch/x86/include/asm/kvm-x86-pmu-ops.h" \
	"$P/arch/x86/include/asm/vmx.h" \
	"$P/arch/x86/include/asm/vmxfeatures.h" \
	"$P/arch/x86/include/asm/svm.h" \
	"$P/arch/x86/include/uapi/asm/kvm.h" \
	"$P/arch/x86/include/uapi/asm/kvm_para.h" \
	"$P/arch/x86/include/uapi/asm/kvm_perf.h" \
	"$P/arch/x86/include/uapi/asm/vmx.h" \
	"$P/arch/x86/include/uapi/asm/svm.h" \
	"$P/include/trace/events/kvm.h"

# Architectural definitions KVM is written against: register numbers, feature
# bits, descriptor and APIC layouts. They are data, not kernel machinery, so
# they come from Linux as they are rather than being retyped into the shim.
# The x86 machinery around them (MSR access, alternatives, the FPU, the APIC
# driver) is linuxkpi's, in kernel/include.
tar -xf "$TAR_PATH" -C "$STAGE_DIR.tmp" --strip-components=1 \
	"$P/arch/x86/include/asm/msr-index.h" \
	"$P/arch/x86/include/asm/sev-common.h" \
	"$P/include/linux/amd-iommu.h" \
	"$P/include/linux/psp-sev.h" \
	"$P/include/uapi/linux/psp-sev.h" \
	"$P/arch/x86/include/asm/cpufeatures.h" \
	"$P/arch/x86/include/asm/trapnr.h" \
	"$P/arch/x86/include/asm/desc_defs.h" \
	"$P/arch/x86/include/asm/apicdef.h" \
	"$P/arch/x86/include/asm/pvclock-abi.h" \
	"$P/arch/x86/include/asm/emulate_prefix.h" \
	"$P/arch/x86/include/asm/posted_intr.h" \
	"$P/arch/x86/include/asm/cpuid/api.h" \
	"$P/arch/x86/include/asm/cpuid/types.h" \
	"$P/arch/x86/include/asm/fpu/xcr.h" \
	"$P/arch/x86/include/asm/mtrr.h" \
	"$P/arch/x86/include/uapi/asm/mtrr.h" \
	"$P/arch/x86/include/uapi/asm/debugreg.h" \
	"$P/arch/x86/include/uapi/asm/processor-flags.h" \
	"$P/arch/x86/include/asm/processor-flags.h" \
	"$P/arch/x86/include/uapi/asm/msr.h" \
	"$P/arch/x86/include/uapi/asm/perf_regs.h" \
	"$P/arch/x86/include/asm/perf_event.h" \
	"$P/arch/x86/include/asm/pvclock.h" \
	"$P/arch/x86/include/asm/user.h" \
	"$P/arch/x86/include/asm/user_64.h" \
	"$P/arch/x86/include/asm/pkru.h" \
	"$P/arch/x86/include/asm/fpu/types.h" \
	"$P/arch/x86/include/asm/vmxfeatures.h" \
	"$P/include/hyperv" \
	"$P/arch/x86/include/asm/xen/cpuid.h" \
	"$P/arch/x86/include/asm/xen/interface.h" \
	"$P/arch/x86/include/asm/xen/interface_64.h" \
	"$P/include/xen/interface/xen.h" \
	"$P/include/xen/interface/vcpu.h" \
	"$P/include/xen/interface/version.h" \
	"$P/include/xen/interface/event_channel.h" \
	"$P/include/xen/interface/sched.h" \
	"$P/arch/x86/include/asm/asm.h" \
	"$P/arch/x86/include/asm/extable_fixup_types.h" \
	"$P/arch/x86/include/asm/irq_vectors.h" \
	"$P/arch/x86/include/asm/mce.h" \
	"$P/arch/x86/include/uapi/asm/mce.h" \
	"$P/arch/x86/include/asm/e820/api.h" \
	"$P/arch/x86/include/asm/e820/types.h" \
	"$P/arch/x86/include/uapi/asm/setup_data.h" \
	"$P/arch/x86/include/asm/ibt.h" \
	"$P/arch/x86/include/asm/debugreg.h" \
	"$P/arch/x86/include/asm/intel_pt.h" \
	"$P/include/uapi/linux/perf_event.h"

mv "$STAGE_DIR.tmp" "$STAGE_DIR"
echo "$STAGE_DIR"
