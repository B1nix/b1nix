#ifndef _UCONTEXT_H
#define _UCONTEXT_H

/* b1nix exposes ucontext_t (the signal-handler third argument) via
 * <sys/ucontext.h>; the bare <ucontext.h> is the conventional include for code
 * that only needs the type (e.g. profilers reading a signal's mcontext). The
 * makecontext/swapcontext family is not provided. */
#include <sys/ucontext.h>

#endif
