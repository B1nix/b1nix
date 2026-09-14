/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_MM_TYPES_H
#define LKPI_LINUX_MM_TYPES_H
#include <linux/mm.h>
#include <linux/types.h>
/* A userspace mapping, as much of it as the core names. b1nix tracks mappings
 * in its own VMA structures; this is the handle the DRM mmap paths are handed,
 * and it is filled in by whoever calls into them. */
/* Operations a driver hangs off a mapping so it can fault pages in and clean
 * up when the mapping goes away. */
struct vm_operations_struct;

struct vm_area_struct {
	unsigned long vm_start;
	unsigned long vm_end;
	unsigned long vm_pgoff;
	unsigned long vm_flags;
	void *vm_private_data;
	struct file *vm_file;
	const struct vm_operations_struct *vm_ops;
	pgprot_t vm_page_prot;
	struct mm_struct *vm_mm;
};

typedef int vm_fault_t;
struct vm_fault {
	struct vm_area_struct *vma;
	unsigned long address;
	unsigned long pgoff;
	struct page *page;
	/* Why the fault was taken and what the handler may do about it. b1nix's
	 * fault path takes no fault under a lock it might have to drop, so the
	 * retry flags are never set — a handler that tests them sees the
	 * blocking case, which is the one it is allowed to take. */
	unsigned int flags;
};

struct vm_operations_struct {
	void (*open)(struct vm_area_struct *area);
	void (*close)(struct vm_area_struct *area);
	vm_fault_t (*fault)(struct vm_fault *vmf);
	int (*access)(struct vm_area_struct *vma, unsigned long addr, void *buf,
	              int len, int write);
	/*
	 * A write is about to happen to a page mapped read-only.
	 *
	 * This is where a filesystem allocates the blocks behind a shared
	 * mapping — a page can be mapped long before anything is written to it. A
	 * NULL here means "writes need no preparation", which for a file-backed
	 * mapping is how a write ends up with nowhere to go.
	 */
	vm_fault_t (*page_mkwrite)(struct vm_fault *vmf);
	vm_fault_t (*pfn_mkwrite)(struct vm_fault *vmf);
	/*
	 * Map the pages around a fault in one go, so a sequential walk does not
	 * fault per page. Purely an optimisation: a NULL costs faults, not
	 * correctness.
	 */
	vm_fault_t (*map_pages)(struct vm_fault *vmf, pgoff_t start_pgoff,
	                        pgoff_t end_pgoff);
};
struct mm_struct;

/*
 * What an mmap_prepare hook sees (6.17): the mapping as it is about to be
 * created, before a VMA exists. A hook adjusts the flags and installs its
 * operations; the mmap path copies them into the VMA it then creates.
 */
struct vm_area_desc {
	struct mm_struct *const mm;
	struct file *const file;
	unsigned long start;
	unsigned long end;
	pgoff_t pgoff;
	struct file *vm_file;
	unsigned long vm_flags;
	pgprot_t page_prot;
	const struct vm_operations_struct *vm_ops;
	void *private_data;
};

#endif
