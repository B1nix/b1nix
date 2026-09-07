/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_FS_LIB_PRELUDE_H
#define LKPI_FS_LIB_PRELUDE_H

/*
 * The force-included prelude for the imported compression libraries.
 *
 * They need everything <linux/types.h> provides and one thing it must not give
 * them: `current`. That header defines it as a macro for the running task, and
 * zstd declares a local variable of the same name — so the macro rewrites the
 * declaration, and the error lands on a line with nothing to do with tasks.
 *
 * Undefining it here rather than passing -Ucurrent, because -U removes only
 * command-line macros and this one comes from a header. The libraries never ask
 * which task they are on; the filesystems above them do, and they include the
 * ordinary prelude.
 *
 * It is the same hazard CLAUDE.md records for `mutex`: a macro over a word this
 * common cannot be scoped to the uses it was meant for.
 */
#include <linux/types.h>

#undef current

#endif
