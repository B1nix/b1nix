/* C++ ABI guard variables for thread-safe function-local statics.
 *
 * These live in their own translation unit (not stdlib.o) on purpose: when a
 * C++ program is linked against libstdc++, libsupc++ already provides
 * __cxa_guard_{acquire,release,abort}. If these were in stdlib.o the linker
 * would drag them in alongside any other stdlib.o symbol (e.g. mallinfo2) and
 * collide with libstdc++ ("multiple definition of __cxa_guard_acquire"). Kept
 * standalone, the linker only pulls them when nothing else defines the symbol
 * (i.e. freestanding C++ with no libstdc++). */

/* Returns 0 on first call, nonzero otherwise. */
int __cxa_guard_acquire(int *guard) {
	if (*guard) return 0;
	return 1;
}

void __cxa_guard_release(int *guard) {
	*guard = 1;
}

void __cxa_guard_abort(int *guard) {
	*guard = 0;
}
