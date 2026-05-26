#ifndef B1NIX_U_ASSERT_H
#define B1NIX_U_ASSERT_H

#ifdef NDEBUG
#define assert(ignore) ((void)0)
#else
void _exit(int status) __attribute__((noreturn));
#define assert(expr) ((expr) ? (void)0 : _exit(139))
#endif

#endif
