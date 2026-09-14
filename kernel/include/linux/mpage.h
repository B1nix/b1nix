/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_MPAGE_H
#define LKPI_LINUX_MPAGE_H

#include <linux/buffer_head.h>

/*
 * Multi-page block I/O.
 *
 * The point of it is one bio for many pages: a read-ahead of sixteen contiguous
 * pages becomes one request instead of sixteen, provided the blocks behind them
 * are contiguous too — which `get_block` is asked, page by page, to say.
 *
 * ext4 uses `mpage_readahead` for its buffered read path. There is no
 * `mpage_writepages` here because ext4 supplies its own.
 */

struct readahead_control;
struct writeback_control;

void mpage_readahead(struct readahead_control *rac, get_block_t get_block);
int mpage_read_folio(struct folio *folio, get_block_t get_block);
int mpage_writepages(struct address_space *mapping,
                     struct writeback_control *wbc, get_block_t get_block);

#endif
