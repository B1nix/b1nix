#ifndef B1NIX_U_SETJMP_H
#define B1NIX_U_SETJMP_H

typedef long jmp_buf[8];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif
