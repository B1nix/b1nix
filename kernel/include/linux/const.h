/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CONST_H
#define LKPI_LINUX_CONST_H
#ifndef __AC
#define __AC(X, Y)  (X##Y)
#define _AC(X, Y)   __AC(X, Y)
#define _AT(T, X)   ((T)(X))
#endif
#ifndef _UL
#define _UL(x)      (_AC(x, UL))
#define _ULL(x)     (_AC(x, ULL))
#endif
#ifndef _BITUL
#define _BITUL(x)   (_UL(1) << (x))
#define _BITULL(x)  (_ULL(1) << (x))
#endif
#ifndef UL
#define UL(x)       (_UL(x))
#define ULL(x)      (_ULL(x))
#endif
#endif
