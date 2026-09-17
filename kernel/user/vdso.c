/*
 * The vDSO, kernel side: the data page, the embedded image, and mapping both
 * into a process at exec.
 *
 * The frames are allocated once and shared by every process. The kernel keeps
 * its own reference to each, so a process unmapping them (or exiting) only
 * drops its own; nothing a process does can free them or write to them:
 *
 *   - both mappings are read-only, and mprotect refuses to make them writable
 *     (sys_mprotect checks vm_area.special);
 *   - a ptrace poke is refused on these frames (vdso_frame_is_shared).
 *
 * Placement: just below the program interpreter's region
 * (USER_VDSO_AREA_TOP), away from the upward-growing mmap arena and from the
 * stack, TLS block and signal trampoline at the top of the address space. With
 * `b1nix.aslr` the pair slides down by a random whole number of pages, the
 * same opt-in that randomises the PIE load base.
 */
#include <b1nix/vdso.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/namespace.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/user.h>
#include <string.h>

/* Generated from build/<arch>/vdso/vdso.so by the Makefile. */
#include "vdso_image.inc"

/* The image is a few kilobytes; this bounds what a broken build could ask the
 * loader to map, not what a working one needs. */
#define VDSO_MAX_PAGES 8

/* How far `b1nix.aslr` may slide the pair down: 2^18 pages, about 1 GiB. */
#define VDSO_ASLR_PAGES (1ull << 18)

#define ELF_PT_LOAD 1
#define ELF_ET_DYN  3

/* Until vdso_init allocates the real page, writers update this copy; the page
 * starts from it. Clock initialisation runs early enough on some paths that
 * dropping those writes would leave the page describing no clock at all. */
static struct vdso_data g_boot_data;
static struct vdso_data *g_data = &g_boot_data;
static spinlock_t g_vdso_lock = SPINLOCK_INIT;

static u64 g_data_frame;
/* The [vvar] page of every task in a time namespace other than the initial one
 * (M123). It never describes a clock, so the vDSO hands every reading to the
 * system call, which applies the namespace's offsets. */
static u64 g_timens_frame;
static u64 g_text_frames[VDSO_MAX_PAGES];
static usize g_text_pages;

struct vdso_data *vdso_write_begin(u64 *flags)
{
	spin_lock_irqsave(&g_vdso_lock, flags);
	struct vdso_data *d = g_data;
	__atomic_store_n(&d->seq, d->seq + 1u, __ATOMIC_RELEASE);
	return d;
}

void vdso_write_end(u64 flags)
{
	struct vdso_data *d = g_data;
	__atomic_store_n(&d->seq, d->seq + 1u, __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&g_vdso_lock, flags);
}

struct elf64_hdr_min {
	u8 e_ident[16];
	u16 e_type;
	u16 e_machine;
	u32 e_version;
	u64 e_entry;
	u64 e_phoff;
	u64 e_shoff;
	u32 e_flags;
	u16 e_ehsize;
	u16 e_phentsize;
	u16 e_phnum;
} __attribute__((packed));

struct elf64_phdr_min {
	u32 p_type;
	u32 p_flags;
	u64 p_offset;
	u64 p_vaddr;
	u64 p_paddr;
	u64 p_filesz;
	u64 p_memsz;
	u64 p_align;
} __attribute__((packed));

/* The build already checks the image; this is the kernel refusing to map
 * something it cannot describe, rather than trusting the build. The mapping
 * is the file verbatim, so what matters is one PT_LOAD at file offset 0 and
 * address 0 that fits in the pages being mapped. */
static int vdso_image_ok(const u8 *img, usize len)
{
	const struct elf64_hdr_min *eh = (const struct elf64_hdr_min *)img;

	if (len < sizeof(*eh) || img[0] != 0x7f || img[1] != 'E' ||
	    img[2] != 'L' || img[3] != 'F' || img[4] != 2 /* ELFCLASS64 */ ||
	    eh->e_type != ELF_ET_DYN ||
	    eh->e_phentsize != sizeof(struct elf64_phdr_min))
		return 0;
	if (eh->e_phoff > len ||
	    eh->e_phnum > (len - eh->e_phoff) / sizeof(struct elf64_phdr_min))
		return 0;

	int loads = 0;
	for (u16 i = 0; i < eh->e_phnum; i++) {
		const struct elf64_phdr_min *ph = (const struct elf64_phdr_min *)
			(img + eh->e_phoff + (u64)i * sizeof(*ph));

		if (ph->p_type != ELF_PT_LOAD)
			continue;
		loads++;
		if (ph->p_offset != 0 || ph->p_vaddr != 0 || ph->p_filesz > len ||
		    ph->p_memsz != ph->p_filesz)
			return 0;
	}
	return loads == 1;
}

void vdso_init(void)
{
	usize len = (usize)vdso_image_len;
	usize pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;

	if (!vdso_image_ok(vdso_image, len) || pages == 0 || pages > VDSO_MAX_PAGES) {
		console_write("vdso: embedded image rejected; processes get no vDSO\n");
		return;
	}

	u64 direct = vmm_direct_map_base();
	u64 data_frame = pmm_alloc_frame();
	if (!data_frame) {
		console_write("vdso: no frame for the data page; processes get no vDSO\n");
		return;
	}
	memset((void *)(usize)(direct + data_frame), 0, PAGE_SIZE);
	u64 timens_frame = pmm_alloc_frame();
	if (!timens_frame) {
		pmm_free_frame(data_frame);
		console_write("vdso: no frame for the time-namespace page; processes get no vDSO\n");
		return;
	}
	{
		struct vdso_data *tp =
		    (struct vdso_data *)(usize)(direct + timens_frame);
		memset(tp, 0, PAGE_SIZE);
		tp->version = VDSO_DATA_VERSION;
		tp->clock_mode = VDSO_CLOCK_SYSCALL;
	}

	for (usize i = 0; i < pages; i++) {
		u64 f = pmm_alloc_frame();

		if (!f) {
			for (usize k = 0; k < i; k++)
				pmm_free_frame(g_text_frames[k]);
			pmm_free_frame(data_frame);
			pmm_free_frame(timens_frame);
			console_write("vdso: no frames for the image; processes get no vDSO\n");
			return;
		}
		u8 *dst = (u8 *)(usize)(direct + f);
		usize off = i * PAGE_SIZE;
		usize chunk = len - off < PAGE_SIZE ? len - off : PAGE_SIZE;

		memset(dst, 0, PAGE_SIZE);
		memcpy(dst, vdso_image + off, chunk);
		g_text_frames[i] = f;
	}

	/* Move the holding copy onto the page and switch writers over, in one
	 * critical section so no update lands on the copy after it was taken. */
	u64 flags;
	spin_lock_irqsave(&g_vdso_lock, &flags);
	struct vdso_data *page = (struct vdso_data *)(usize)(direct + data_frame);
	*page = g_boot_data;
	page->seq = 0;
	page->version = VDSO_DATA_VERSION;
	g_data = page;
	g_data_frame = data_frame;
	g_timens_frame = timens_frame;
	g_text_pages = pages;
	spin_unlock_irqrestore(&g_vdso_lock, flags);

	console_write("vdso: ");
	console_write_dec(len);
	console_write("-byte image in ");
	console_write_dec(pages);
	console_write(" page(s), clock mode ");
	console_write_dec(page->clock_mode);
	console_write("\n");
}

u64 vdso_text_size(void)
{
	return (u64)g_text_pages * PAGE_SIZE;
}

u64 vdso_choose_base(void)
{
	if (!g_text_pages)
		return 0;

	u64 slide = 0;
	if (bootinfo_has_flag("b1nix.aslr"))
		slide = (kernel_random_u64() % VDSO_ASLR_PAGES) * PAGE_SIZE;

	/* [vvar][vdso...] ending one guard page below the area's top. */
	u64 end = USER_VDSO_AREA_TOP - PAGE_SIZE - slide;
	return end - vdso_text_size();
}

/* The data frame a task's [vvar] must show. */
static u64 vdso_vvar_frame_for(const struct task *t)
{
	return namespace_task_id(t, NS_TIME) ? g_timens_frame : g_data_frame;
}

void vdso_timens_update(struct task *t)
{
	if (!t || !g_text_pages || !t->pml4_phys)
		return;
	u64 want = vdso_vvar_frame_for(t);
	for (struct vm_area *v = t->vma_list; v; v = v->next) {
		if (v->special != VMA_SPECIAL_VVAR)
			continue;
		u64 have = paging_user_frame(t->pml4_phys, v->start);
		if (have == want)
			return;
		pmm_ref_frame(want);
		paging_set_page_in_space(t->pml4_phys, v->start, want,
		                         vmm_user_flags_from_prot(PROT_READ) | VMM_PRESENT);
		if (have)
			pmm_free_frame(have);
		return;
	}
}

int vdso_map_current(struct user_loaded_image *image)
{
	u64 base = image ? image->vdso_base : 0;

	if (!base || !g_text_pages || !current_task)
		return 0;

	u64 vvar = base - PAGE_SIZE;
	u64 end = base + vdso_text_size();
	struct vm_area *vvar_vma = kzalloc(sizeof(struct vm_area));
	struct vm_area *text_vma = kzalloc(sizeof(struct vm_area));

	/* Both descriptions exist before anything is mapped, so there is no
	 * half-installed state to undo. */
	if (!vvar_vma || !text_vma) {
		kfree(vvar_vma);
		kfree(text_vma);
		return -ENOMEM;
	}

	/* Each mapping takes its own reference; the kernel's stays, so unmapping
	 * or exiting never frees a frame every other process still uses. */
	u64 vvar_frame = vdso_vvar_frame_for(current_task);
	vmm_map_page(vvar, vvar_frame,
	             vmm_user_flags_from_prot(PROT_READ) | VMM_PRESENT);
	pmm_ref_frame(vvar_frame);
	for (usize i = 0; i < g_text_pages; i++) {
		vmm_map_page(base + (u64)i * PAGE_SIZE, g_text_frames[i],
		             vmm_user_flags_from_prot(PROT_READ | PROT_EXEC) | VMM_PRESENT);
		pmm_ref_frame(g_text_frames[i]);
	}

	vvar_vma->start = vvar;
	vvar_vma->end = base;
	vvar_vma->prot = PROT_READ;
	vvar_vma->flags = MAP_PRIVATE;
	vvar_vma->special = VMA_SPECIAL_VVAR;
	vma_insert(current_task, vvar_vma);

	text_vma->start = base;
	text_vma->end = end;
	text_vma->prot = PROT_READ | PROT_EXEC;
	text_vma->flags = MAP_PRIVATE;
	text_vma->special = VMA_SPECIAL_VDSO;
	vma_insert(current_task, text_vma);
	return 0;
}

int vdso_frame_is_shared(u64 frame)
{
	frame &= ~(u64)(PAGE_SIZE - 1);
	if (!g_text_pages)
		return 0;
	if (frame == g_data_frame || frame == g_timens_frame)
		return 1;
	for (usize i = 0; i < g_text_pages; i++)
		if (frame == g_text_frames[i])
			return 1;
	return 0;
}
