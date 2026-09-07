/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_ERRSEQ_H
#define LKPI_LINUX_ERRSEQ_H

#include <linux/types.h>

/*
 * Defined here as well as in <linux/fs.h>, guarded: this header is included
 * from fs.h BEFORE fs.h's own typedef is reached, and it is also included on
 * its own by code that never sees fs.h.
 */
#ifndef LKPI_ERRSEQ_T_DEFINED
#define LKPI_ERRSEQ_T_DEFINED
typedef u32 errseq_t;
#endif

/*
 * A writeback error, encoded so each observer sees it exactly once.
 *
 * The low bits hold the errno and the high bits a counter. A reader keeps its
 * own last-seen value; `errseq_check_and_advance` returns the error only if the
 * recorded value has moved since, and updates the reader's copy. `errseq_check`
 * is the same question without consuming — the two are not interchangeable, and
 * using the consuming one where a peek was meant makes the error vanish for
 * whoever should have received it.
 */

errseq_t errseq_set(errseq_t *eseq, int err);
errseq_t errseq_sample(errseq_t *eseq);
int errseq_check(errseq_t *eseq, errseq_t since);
int errseq_check_and_advance(errseq_t *eseq, errseq_t *since);

#endif
