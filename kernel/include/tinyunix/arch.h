#ifndef TINYUNIX_ARCH_H
#define TINYUNIX_ARCH_H

void arch_init(void);
void arch_halt(void) __attribute__((noreturn));

#endif
