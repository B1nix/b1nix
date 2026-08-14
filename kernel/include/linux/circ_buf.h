/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CIRC_BUF_H
#define LKPI_LINUX_CIRC_BUF_H
/* A ring described by head, tail and a power-of-two size. One producer and one
 * consumer need no lock between them, which is why a driver uses this for a
 * hardware queue. */
struct circ_buf { char *buf; int head; int tail; };
#define CIRC_CNT(head, tail, size) (((head) - (tail)) & ((size) - 1))
#define CIRC_SPACE(head, tail, size) CIRC_CNT((tail), ((head) + 1), (size))
#define CIRC_CNT_TO_END(head, tail, size)                        \
	({                                                           \
		int end = (size) - (tail);                               \
		int n = ((head) + end) & ((size) - 1);                   \
		n < end ? n : end;                                       \
	})
#endif
