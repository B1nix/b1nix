#ifndef B1NIX_PANIC_H
#define B1NIX_PANIC_H

void panic(const char *message) __attribute__((noreturn));

#define ASSERT(condition) \
	do { \
		if (!(condition)) { \
			panic("assertion failed: " #condition); \
		} \
	} while (0)

#endif
