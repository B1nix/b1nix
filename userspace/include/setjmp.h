#ifndef B1NIX_U_SETJMP_H
#define B1NIX_U_SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef long jmp_buf[8];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

/* sigsetjmp/siglongjmp — setjmp variants that optionally save and restore the
 * signal mask (POSIX). bash uses these for its top-level interrupt handling.
 *
 * sigsetjmp must capture the *caller's* machine context, so it is a macro that
 * invokes the real (assembly) setjmp inline at the call site; a C-function
 * wrapper would instead capture the wrapper's frame, which is gone by the time
 * siglongjmp fires. The helper records the savemask flag and stashes the
 * current signal mask before setjmp runs. siglongjmp is noreturn — longjmp does
 * the actual stack switch — so it can be an ordinary function. */
struct __sigjmp_buf {
	jmp_buf __jb;
	int __savemask;
	unsigned long long __mask;
};
typedef struct __sigjmp_buf sigjmp_buf[1];

int __sigsetjmp_save(struct __sigjmp_buf *env, int savemask);
void siglongjmp(sigjmp_buf env, int val) __attribute__((noreturn));

#define sigsetjmp(env, savemask) \
	(__sigsetjmp_save((env), (savemask)), setjmp((env)[0].__jb))

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif
