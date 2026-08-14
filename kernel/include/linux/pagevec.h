/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PAGEVEC_H
#define LKPI_LINUX_PAGEVEC_H
#include <linux/mm.h>
/* A small batch of pages, so a loop can amortise the page-cache lock over
 * several pages instead of taking it per page. */
#define PAGEVEC_SIZE 15
struct pagevec { unsigned char nr; struct page *pages[PAGEVEC_SIZE]; };
static inline void pagevec_init(struct pagevec *pvec) { pvec->nr = 0; }
static inline unsigned pagevec_count(struct pagevec *pvec) { return pvec->nr; }
static inline unsigned pagevec_space(struct pagevec *pvec)
{ return PAGEVEC_SIZE - pvec->nr; }
static inline unsigned pagevec_add(struct pagevec *pvec, struct page *page)
{ pvec->pages[pvec->nr++] = page; return pagevec_space(pvec); }
#endif
