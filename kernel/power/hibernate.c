/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Hibernation (M135). See include/b1nix/hibernate.h.
 *
 * The image is Linux's swsusp in outline, sized for this kernel:
 *
 *   snapshot  with userspace frozen, the other CPUs parked and interrupts off,
 *             every used frame of RAM (the AVAILABLE regions of the boot
 *             memory map whose frame the allocator has handed out) is copied
 *             into a free one, so what is written is one instant of the whole
 *             machine rather than a machine that kept moving while it was
 *             being written;
 *   write     the copy goes to the resume device -- a swap area, whose first
 *             page keeps its label and trades SWAPSPACE2 for B1NXHIBE --
 *             behind a header, followed by the frame number of every page;
 *   restore   the next boot, before it mounts anything, reads the pages into
 *             frames the image does not use, then (x86_hib_restore) copies
 *             each one home from a stack and page tables in such frames and
 *             jumps into the kernel that wrote the image, which finds itself
 *             returning from x86_hib_setjmp for the second time.
 *
 * The boot kernel and the image are the same binary; the header says which
 * binary it was (a hash of the kernel text) and a different one refuses the
 * image. The devices the boot kernel brought up are taken back by the image's
 * own drivers through the same resume callbacks an S3 uses, because to them
 * the machine has been reset -- which it has.
 */

#include <b1nix/hibernate.h>

#include <b1nix/arch.h>
#include <b1nix/blk.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/io.h>
#include <b1nix/iommu.h>
#include <b1nix/kprintf.h>
#include <b1nix/mm.h>
#include <b1nix/page_cache.h>
#include <b1nix/pci.h>
#include <b1nix/sched.h>
#include <b1nix/suspend.h>
#include <b1nix/types.h>
#include <b1nix/vfs.h>

#include <stdio.h>
#include <string.h>

enum { MODE_SHUTDOWN, MODE_REBOOT };
static int g_mode = MODE_SHUTDOWN;
static char g_resume_spec[64];

const char *hibernate_disk_modes(void) {
  return g_mode == MODE_REBOOT ? "shutdown [reboot]" : "[shutdown] reboot";
}

int hibernate_set_disk_mode(const char *name) {
  if (!strcmp(name, "shutdown"))
    g_mode = MODE_SHUTDOWN;
  else if (!strcmp(name, "reboot"))
    g_mode = MODE_REBOOT;
  else
    return -EINVAL;
  return 0;
}

const char *hibernate_resume_device(void) { return g_resume_spec; }

int hibernate_set_resume_device(const char *spec) {
  usize n = strlen(spec);

  if (n == 0 || n >= sizeof(g_resume_spec))
    return -EINVAL;
  memcpy(g_resume_spec, spec, n + 1);
  return 0;
}

#if defined(__x86_64__)

#define HIB_MAGIC "B1NXHIB1"
#define HIB_SIG "B1NXHIBE"
#define SWAP_SIG "SWAPSPACE2"
#define SWAP_SIG_OFF (PAGE_SIZE - 10)
#define SWAP_LABEL_OFF 1052
#define HIB_VERSION 1u
#define IO_PAGES 16u /* pages per transfer */

struct hib_jmp {
  u64 rbx, rbp, r12, r13, r14, r15, rsp, rip, cr3;
};

struct hib_header {
  char magic[8];
  u32 version, page_size;
  u64 nr_pages;
  u64 text_hash;
  u64 max_phys;
  u64 jmp_va;
  u64 pfn_page;  /* first device page of the frame list */
  u64 data_page; /* first device page of the data */
  u64 sum;       /* of every field above */
};

int x86_hib_setjmp(struct hib_jmp *j);
void x86_hib_restore(u64 temp_cr3, u64 stack_top, u64 chain, struct hib_jmp *j)
    __attribute__((noreturn));
void x86_hib_save_cpu(void);
void x86_hib_restore_cpu(void);
int pmm_frame_used(u64 phys);
void pmm_frame_adopt(u64 phys);
u64 pmm_max_phys(void);
int suspend_devices_suspend(void);
void acpi_poweroff(void);
extern char __kernel_text_start[], __kernel_text_end[];

/* In the image as much as in the boot kernel, at the same address: the boot
 * kernel's restore jumps through the image's copy of it. */
static struct hib_jmp g_hib_jmp;

static void *dm(u64 phys) { return (void *)(usize)(phys + DIRECT_MAP_BASE); }

/* Which binary this is. The image can only be continued by the code that
 * wrote it. */
static u64 g_text_hash;

/* Taken once, at boot (the resume check runs early on every boot that names a
 * resume device): the text a kprobe or ftrace patches later is still the same
 * binary. */
static u64 text_hash(void) {
  const u8 *p = (const u8 *)__kernel_text_start;
  const u8 *e = (const u8 *)__kernel_text_end;
  u64 h = 0xcbf29ce484222325ull;

  if (g_text_hash)
    return g_text_hash;
  for (; p + 8 <= e; p += 8) {
    h ^= *(const u64 *)p;
    h *= 0x100000001b3ull;
  }
  g_text_hash = h;
  return h;
}

static u64 header_sum(const struct hib_header *h) {
  const u64 *w = (const u64 *)h;
  u64 s = 0x9e3779b97f4a7c15ull;

  for (usize i = 0; i < __builtin_offsetof(struct hib_header, sum) / 8; i++)
    s = (s ^ w[i]) * 0x100000001b3ull;
  return s;
}

/* ── the resume device ──────────────────────────────────────────────────── */

static struct block_device *resume_dev(void) {
  const char *spec = g_resume_spec;

  if (!spec[0])
    return 0;
  if (!strncmp(spec, "/dev/", 5))
    spec += 5;
  if (strncmp(spec, "LABEL=", 6) != 0)
    return blk_get(spec);
  /* A swap area by its label, as blkid would find it. */
  spec += 6;
  for (usize i = 0; i < blk_count(); i++) {
    struct block_device *d = blk_at(i);
    u8 *page;
    int hit;

    if (!d || !d->read_blocks || d->block_size != 512 || blk_is_partition(d))
      continue;
    page = kmalloc(PAGE_SIZE);
    if (!page)
      return 0;
    hit = d->read_blocks(d, 0, PAGE_SIZE / 512, page) >= 0 &&
          (!memcmp(page + SWAP_SIG_OFF, SWAP_SIG, 10) ||
           !memcmp(page + SWAP_SIG_OFF, HIB_SIG, 8)) &&
          !strncmp((const char *)page + SWAP_LABEL_OFF, spec, 16);
    kfree(page);
    if (hit)
      return d;
  }
  return 0;
}

/* Whole pages, IO_PAGES at a time at most. 0, or -1: the drivers answer the
 * number of blocks moved on success. */
static int dev_pages(struct block_device *d, u64 page, u32 n, void *buf,
                     int write) {
  u32 per = PAGE_SIZE / 512;
  int rc = write ? d->write_blocks(d, page * per, n * per, buf)
                 : d->read_blocks(d, page * per, n * per, buf);

  return rc < 0 ? -1 : 0;
}

/* ── what to save ───────────────────────────────────────────────────────── */

/* Calls fn(frame) for every used frame of RAM, in address order. */
static u64 for_each_used(void (*fn)(u64 frame, void *ctx), void *ctx) {
  const struct boot_info *bi = bootinfo_get();
  u64 max = pmm_max_phys();
  u64 n = 0;

  for (usize r = 0; bi && r < bi->memory_region_count; r++) {
    const struct boot_memory_region *m = &bi->memory_regions[r];
    u64 a, e;

    if (m->type != BOOT_MEMORY_AVAILABLE)
      continue;
    a = (m->base + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
    e = (m->base + m->length) & ~(u64)(PAGE_SIZE - 1);
    if (e > max)
      e = max;
    for (u64 f = a; f < e; f += PAGE_SIZE)
      if (pmm_frame_used(f)) {
        if (fn)
          fn(f, ctx);
        n++;
      }
  }
  return n;
}

/* ── the snapshot ───────────────────────────────────────────────────────── */

/* A list of frame pairs in pages of their own: { next, count, pairs[255] }.
 * The same layout the restore walks, so the writer and the restore share it. */
#define PAIRS_PER_PAGE 255u

struct pair_page {
  u64 next; /* direct-map address of the next page, or 0 */
  u64 count;
  struct {
    u64 a, b;
  } p[PAIRS_PER_PAGE];
};

struct snap {
  u8 *copyset;      /* bit per frame: a copy, not to be saved */
  u64 copyset_phys;
  usize copyset_frames;
  struct pair_page *head, *tail; /* (source frame, copy frame) */
  u64 n;
};

static struct snap g_snap;

static int pair_add(struct pair_page **head, struct pair_page **tail, u64 a,
                    u64 b, u64 (*alloc)(void)) {
  struct pair_page *t = *tail;

  if (!t || t->count == PAIRS_PER_PAGE) {
    u64 f = alloc();
    struct pair_page *np;

    if (!f)
      return -ENOMEM;
    np = dm(f);
    np->next = 0;
    np->count = 0;
    if (t)
      t->next = (u64)(usize)np;
    else
      *head = np;
    *tail = t = np;
  }
  t->p[t->count].a = a;
  t->p[t->count].b = b;
  t->count++;
  return 0;
}

static void snap_mark_copy(u64 frame) {
  u64 i = frame / PAGE_SIZE;

  g_snap.copyset[i / 8] |= (u8)(1u << (i % 8));
}

static int snap_is_copy(u64 frame) {
  u64 i = frame / PAGE_SIZE;

  return (g_snap.copyset[i / 8] >> (i % 8)) & 1;
}

struct copy_ctx {
  struct pair_page *walk; /* the pages of pre-allocated copy frames */
  u64 idx;
  u64 done;
  int overflow;
};

static void snap_copy_one(u64 frame, void *vctx) {
  struct copy_ctx *c = vctx;

  if (snap_is_copy(frame) || c->overflow)
    return;
  while (c->walk && c->idx >= c->walk->count) {
    c->walk = (struct pair_page *)(usize)c->walk->next;
    c->idx = 0;
  }
  if (!c->walk) {
    c->overflow = 1;
    return;
  }
  c->walk->p[c->idx].a = frame;
  memcpy(dm(c->walk->p[c->idx].b), dm(frame), PAGE_SIZE);
  c->idx++;
  c->done++;
}

static void snap_free(void) {
  /* The copies, then the list and the set; the list pages are ordinary
   * allocations, the copies are marked in the set. */
  for (struct pair_page *p = g_snap.head; p;) {
    struct pair_page *next = (struct pair_page *)(usize)p->next;

    for (u64 k = 0; k < p->count; k++)
      if (p->p[k].b) {
        /* After a resume a copy holds what the boot kernel left there. */
        pmm_frame_adopt(p->p[k].b);
        pmm_free_frame(p->p[k].b);
      }
    pmm_free_frame((u64)(usize)p - DIRECT_MAP_BASE);
    p = next;
  }
  if (g_snap.copyset_phys)
    for (usize k = 0; k < g_snap.copyset_frames; k++)
      pmm_free_frame(g_snap.copyset_phys + k * PAGE_SIZE);
  memset(&g_snap, 0, sizeof(g_snap));
}

/* Interrupts off, one CPU, nothing else running. */
static int snapshot(void) {
  u64 used = for_each_used(0, 0);
  u64 max = pmm_max_phys();
  u64 want, got = 0;
  struct copy_ctx c = {0};

  memset(&g_snap, 0, sizeof(g_snap));
  g_snap.copyset_frames = (usize)((max / PAGE_SIZE / 8 + PAGE_SIZE - 1) / PAGE_SIZE);
  g_snap.copyset_phys = pmm_alloc_frames(g_snap.copyset_frames);
  if (!g_snap.copyset_phys)
    return -ENOMEM;
  g_snap.copyset = dm(g_snap.copyset_phys);
  memset(g_snap.copyset, 0, g_snap.copyset_frames * PAGE_SIZE);
  /* One copy per used frame, plus what this function allocates for itself
   * (the list pages); asked for up front, so the set being copied does not
   * move once copying starts. */
  want = used + used / PAIRS_PER_PAGE + 64;
  if (pmm_free_frame_count() < want + want / PAIRS_PER_PAGE + 16) {
    kprintf(LOGLEVEL_WARNING, "hibernate",
            "%lu pages in use and %lu free: not enough to take a copy",
            (unsigned long)used, (unsigned long)pmm_free_frame_count());
    snap_free();
    return -ENOMEM;
  }
  for (; got < want; got++) {
    u64 f = pmm_alloc_frame();

    if (!f || pair_add(&g_snap.head, &g_snap.tail, 0, f, pmm_alloc_frame) != 0) {
      if (f)
        pmm_free_frame(f);
      snap_free();
      return -ENOMEM;
    }
    snap_mark_copy(f);
  }
  c.walk = g_snap.head;
  for_each_used(snap_copy_one, &c);
  if (c.overflow) {
    snap_free();
    return -ENOMEM;
  }
  g_snap.n = c.done;
  return 0;
}

/* ── writing it ─────────────────────────────────────────────────────────── */

static int write_image(struct block_device *d) {
  u8 *buf = kmalloc(IO_PAGES * PAGE_SIZE);
  u64 *pfns;
  struct hib_header *h;
  u64 pfn_pages = (g_snap.n * 8 + PAGE_SIZE - 1) / PAGE_SIZE;
  u64 need = 2 + pfn_pages + g_snap.n;
  u64 pg;
  int rc = -EIO;

  if (!buf)
    return -ENOMEM;
  if (d->block_count / (PAGE_SIZE / 512) < need) {
    kprintf(LOGLEVEL_WARNING, "hibernate",
            "the resume device holds %lu pages, the image needs %lu",
            (unsigned long)(d->block_count / (PAGE_SIZE / 512)),
            (unsigned long)need);
    kfree(buf);
    return -ENOSPC;
  }
  /* The frame list, then the pages, then the header, and the signature
   * last: an image that was not written whole is never taken for one. */
  pfns = (u64 *)buf;
  pg = 2;
  {
    u64 k = 0, slot = 0;

    for (struct pair_page *p = g_snap.head; p; p = (struct pair_page *)(usize)p->next)
      for (u64 j = 0; j < p->count && k < g_snap.n; j++, k++) {
        pfns[slot++] = p->p[j].a;
        if (slot == IO_PAGES * PAGE_SIZE / 8) {
          if (dev_pages(d, pg, IO_PAGES, buf, 1) != 0)
            goto out;
          pg += IO_PAGES;
          slot = 0;
        }
      }
    if (slot) {
      u32 n = (u32)((slot * 8 + PAGE_SIZE - 1) / PAGE_SIZE);

      memset((u8 *)pfns + slot * 8, 0, n * PAGE_SIZE - slot * 8);
      if (dev_pages(d, pg, n, buf, 1) != 0)
        goto out;
      pg += n;
    }
  }
  {
    u64 k = 0, fill = 0, data_page = 2 + pfn_pages;

    pg = data_page;
    for (struct pair_page *p = g_snap.head; p; p = (struct pair_page *)(usize)p->next)
      for (u64 j = 0; j < p->count && k < g_snap.n; j++, k++) {
        memcpy(buf + fill * PAGE_SIZE, dm(p->p[j].b), PAGE_SIZE);
        if (++fill == IO_PAGES) {
          if (dev_pages(d, pg, IO_PAGES, buf, 1) != 0)
            goto out;
          pg += IO_PAGES;
          fill = 0;
        }
      }
    if (fill && dev_pages(d, pg, (u32)fill, buf, 1) != 0)
      goto out;
  }
  memset(buf, 0, PAGE_SIZE);
  h = (struct hib_header *)buf;
  memcpy(h->magic, HIB_MAGIC, 8);
  h->version = HIB_VERSION;
  h->page_size = PAGE_SIZE;
  h->nr_pages = g_snap.n;
  h->text_hash = text_hash();
  h->max_phys = pmm_max_phys();
  h->jmp_va = (u64)(usize)&g_hib_jmp;
  h->pfn_page = 2;
  h->data_page = 2 + pfn_pages;
  h->sum = header_sum(h);
  if (dev_pages(d, 1, 1, buf, 1) != 0)
    goto out;
  if (dev_pages(d, 0, 1, buf, 0) != 0)
    goto out;
  memcpy(buf + SWAP_SIG_OFF, HIB_SIG "\0\0", 10);
  if (dev_pages(d, 0, 1, buf, 1) != 0)
    goto out;
  if (d->flush)
    (void)d->flush(d);
  kprintf(LOGLEVEL_INFO, "hibernate", "image written: %lu pages to %s",
          (unsigned long)g_snap.n, d->name);
  rc = 0;
out:
  kfree(buf);
  return rc;
}

/* The swap area is a swap area again once its image has been used. */
static void clear_signature(struct block_device *d) {
  u8 *page = kmalloc(PAGE_SIZE);

  if (!page)
    return;
  if (dev_pages(d, 0, 1, page, 0) == 0 &&
      !memcmp(page + SWAP_SIG_OFF, HIB_SIG, 8)) {
    memcpy(page + SWAP_SIG_OFF, SWAP_SIG, 10);
    (void)dev_pages(d, 0, 1, page, 1);
    if (d->flush)
      (void)d->flush(d);
  }
  kfree(page);
}

static void machine_off(void) {
  if (g_mode == MODE_REBOOT) {
    console_write("hibernate: image saved, rebooting\n");
    while (inb(0x64) & 0x02)
      ;
    outb(0x64, 0xFE);
  } else {
    console_write("hibernate: image saved, powering off\n");
    acpi_poweroff();
  }
  for (;;)
    __asm__ volatile("cli; hlt");
}

/* ── the whole of it ────────────────────────────────────────────────────── */

int hibernate_available(void) { return resume_dev() != 0 && !iommu_active(); }

const char *hibernate_why_not(void) {
  if (iommu_active())
    return "an IOMMU is translating DMA, and nothing restores it";
  if (!g_resume_spec[0])
    return "no resume device (resume= or /sys/power/resume)";
  return "the resume device is not there";
}

int hibernate_enter(void) {
  struct block_device *d = resume_dev();
  int rc;

  if (!d || iommu_active())
    return -ENODEV;
  /* What the image does not have to carry: the page cache's clean pages
   * are on disk already, and the dirty ones go there now. */
  (void)vfs_sync();
  page_cache_evict((usize)-1);

  /* Userspace first, then the other CPUs: see suspend_enter_s3. */
  rc = sched_freeze_userspace(5000);
  if (rc < 0)
    return rc;
  rc = sched_park_secondary_cpus(5000);
  if (rc < 0) {
    sched_thaw_userspace();
    return -EBUSY;
  }
  if (suspend_devices_suspend() != 0) {
    sched_thaw_userspace();
    sched_unpark_secondary_cpus();
    return -EBUSY;
  }
  if (current_task)
    arch_fpu_save(current_task->fpu_state);
  interrupts_disable();
  x86_hib_save_cpu();

  if (x86_hib_setjmp(&g_hib_jmp) == 0) {
    /* The snapshot. */
    rc = snapshot();
    interrupts_enable();
    if (rc == 0) {
      kprintf(LOGLEVEL_INFO, "hibernate", "snapshot: %lu pages",
              (unsigned long)g_snap.n);
      rc = write_image(d);
      if (rc == 0)
        machine_off();
      snap_free();
    }
    (void)suspend_resume_devices();
    sched_unpark_secondary_cpus();
    sched_thaw_userspace();
    return rc;
  }

  /* The image, continued. Interrupts are off, the processor is the boot
   * kernel's, the devices are the boot kernel's too. */
  x86_hib_restore_cpu();
  if (current_task)
    arch_fpu_restore(current_task->fpu_state);
  interrupts_enable();
  console_write("hibernate: resumed from the image\n");
  snap_free();
  (void)suspend_resume_devices();
  clear_signature(d);
  sched_unpark_secondary_cpus();
  sched_thaw_userspace();
  return 0;
}

/* ── at boot ────────────────────────────────────────────────────────────── */

/* Frames the image does not name: the only ones the restore may stand on. */
static u8 *g_imgset;
static u64 g_imgset_bytes;

static int in_image(u64 frame) {
  u64 i = frame / PAGE_SIZE;

  return i / 8 < g_imgset_bytes && ((g_imgset[i / 8] >> (i % 8)) & 1);
}

static u64 safe_alloc(void) {
  for (int tries = 0; tries < 1 << 20; tries++) {
    u64 f = pmm_alloc_frame();

    if (!f)
      return 0;
    if (!in_image(f))
      return f;
    /* Kept allocated: the boot kernel must not hand it to anything else,
     * and the image is about to overwrite it anyway. */
  }
  return 0;
}

static u64 safe_zeroed(void) {
  u64 f = safe_alloc();

  if (f)
    memset(dm(f), 0, PAGE_SIZE);
  return f;
}

/* Page tables for the copy: the direct map and the kernel's own window, in
 * safe frames, 2 MiB pages throughout. */
static u64 temp_tables(u64 max_phys) {
  u64 pml4 = safe_zeroed(), pdpt = safe_zeroed(), kpdpt = safe_zeroed();
  u64 kpd = safe_zeroed();
  u64 *t4, *t3;

  if (!pml4 || !pdpt || !kpdpt || !kpd)
    return 0;
  t4 = dm(pml4);
  t3 = dm(pdpt);
  for (u64 g = 0; g * (1ull << 30) < max_phys && g < 512; g++) {
    u64 pd = safe_zeroed();
    u64 *t2;

    if (!pd)
      return 0;
    t2 = dm(pd);
    for (u64 k = 0; k < 512; k++)
      t2[k] = (g * (1ull << 30) + k * (2ull << 20)) | 0x83; /* P W PS */
    t3[g] = pd | 0x3;
  }
  t4[(DIRECT_MAP_BASE >> 39) & 511] = pdpt | 0x3;
  {
    u64 *kt3 = dm(kpdpt), *kt2 = dm(kpd);

    for (u64 k = 0; k < 512; k++)
      kt2[k] = (KSYM_TO_PHYS(KERNEL_VMA + k * (2ull << 20)) & ~((2ull << 20) - 1)) |
               0x83;
    kt3[(KERNEL_VMA >> 30) & 511] = kpd | 0x3;
    t4[(KERNEL_VMA >> 39) & 511] = kpdpt | 0x3;
  }
  return pml4;
}

void hibernate_resume_from_disk(void) {
  char spec[64];
  struct block_device *d;
  u8 *buf;
  struct hib_header h;
  u64 *pfns = 0;
  struct pair_page *head = 0, *tail = 0;
  u64 cr3, stack;

  if (!bootinfo_get_kv("resume", spec, sizeof(spec)) ||
      hibernate_set_resume_device(spec) != 0)
    return;
  (void)text_hash(); /* the binary's, before anything patches it */
  d = resume_dev();
  if (!d)
    return;
  buf = kmalloc(IO_PAGES * PAGE_SIZE);
  if (!buf)
    return;
  if (dev_pages(d, 0, 1, buf, 0) != 0 || memcmp(buf + SWAP_SIG_OFF, HIB_SIG, 8)) {
    kfree(buf);
    return; /* a swap area with no image in it: the ordinary boot */
  }
  if (dev_pages(d, 1, 1, buf, 0) != 0)
    goto refuse;
  memcpy(&h, buf, sizeof(h));
  if (memcmp(h.magic, HIB_MAGIC, 8) || h.version != HIB_VERSION ||
      h.page_size != PAGE_SIZE || h.sum != header_sum(&h)) {
    console_write("hibernate: the image header is damaged\n");
    goto refuse;
  }
  if (h.text_hash != text_hash() || h.jmp_va != (u64)(usize)&g_hib_jmp ||
      h.max_phys != pmm_max_phys()) {
    console_write("hibernate: the image was written by another kernel or on "
                  "another machine\n");
    goto refuse;
  }
  kprintf(LOGLEVEL_INFO, "hibernate", "restoring %lu pages from %s",
          (unsigned long)h.nr_pages, d->name);

  /* The frame list, and the set of frames it names. */
  g_imgset_bytes = h.max_phys / PAGE_SIZE / 8 + 1;
  {
    usize frames = (usize)((g_imgset_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    u64 p = pmm_alloc_frames(frames);
    usize pf = (usize)((h.nr_pages * 8 + PAGE_SIZE - 1) / PAGE_SIZE);
    u64 q;

    if (!p)
      goto refuse;
    g_imgset = dm(p);
    memset(g_imgset, 0, frames * PAGE_SIZE);
    q = pmm_alloc_frames(pf);
    if (!q)
      goto refuse;
    pfns = dm(q);
    for (u64 k = 0; k < pf; k += IO_PAGES) {
      u32 n = (u32)(pf - k < IO_PAGES ? pf - k : IO_PAGES);

      if (dev_pages(d, h.pfn_page + k, n, (u8 *)pfns + k * PAGE_SIZE, 0) != 0)
        goto refuse;
    }
    for (u64 k = 0; k < h.nr_pages; k++) {
      u64 i = pfns[k] / PAGE_SIZE;

      if (pfns[k] >= h.max_phys)
        goto refuse;
      g_imgset[i / 8] |= (u8)(1u << (i % 8));
    }
  }

  /* Every page into a frame the image does not use, and its destination. */
  for (u64 k = 0; k < h.nr_pages; k += IO_PAGES) {
    u32 n = (u32)(h.nr_pages - k < IO_PAGES ? h.nr_pages - k : IO_PAGES);

    if (dev_pages(d, h.data_page + k, n, buf, 0) != 0)
      goto refuse;
    for (u32 j = 0; j < n; j++) {
      u64 f = safe_alloc();

      if (!f || pair_add(&head, &tail, pfns[k + j] + DIRECT_MAP_BASE,
                         f + DIRECT_MAP_BASE, safe_alloc) != 0) {
        console_write("hibernate: not enough free memory to stage the image\n");
        goto refuse;
      }
      memcpy(dm(f), buf + (usize)j * PAGE_SIZE, PAGE_SIZE);
    }
  }
  cr3 = temp_tables(h.max_phys);
  stack = safe_zeroed();
  if (!cr3 || !stack)
    goto refuse;
  console_write("hibernate: jumping into the image\n");
  interrupts_disable();
  pci_quiesce_all();
  x86_hib_restore(cr3, stack + DIRECT_MAP_BASE + PAGE_SIZE, (u64)(usize)head,
                  &g_hib_jmp);

refuse:
  /* The boot continues without the image; what was staged is simply left
   * allocated -- this is a boot, and it is early. The signature goes, so the
   * next boot does not try the same image again. */
  clear_signature(d);
  kfree(buf);
}

#else /* !__x86_64__ */

int hibernate_available(void) { return 0; }
const char *hibernate_why_not(void) {
  return "not implemented on this architecture";
}
int hibernate_enter(void) { return -EOPNOTSUPP; }
void hibernate_resume_from_disk(void) {}

#endif
