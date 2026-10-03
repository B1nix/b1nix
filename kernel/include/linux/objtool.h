/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_OBJTOOL_H
#define LKPI_LINUX_OBJTOOL_H
/* No objtool pass runs over this kernel; its annotations mean nothing. */
#define STACK_FRAME_NON_STANDARD(func)
#define STACK_FRAME_NON_STANDARD_FP(func)
#define ANNOTATE_NOENDBR
#define ANNOTATE_RETPOLINE_SAFE
#define ANNOTATE_INTRA_FUNCTION_CALL
#define ANNOTATE_UNRET_SAFE
#define ANNOTATE_NOENDBR_SYM(sym)
#define VALIDATE_UNRET_BEGIN
#endif
