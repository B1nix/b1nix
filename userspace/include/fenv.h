#ifndef _FENV_H
#define _FENV_H

/* Minimal <fenv.h>. b1nix runs userspace with a fixed FPU rounding mode and
 * does not surface FP exception flags, so the exception calls are no-ops and
 * rounding queries report round-to-nearest. Enough for ports (Mesa) that probe
 * fenv but tolerate a stub. */
#ifdef __cplusplus
extern "C" {
#endif

#define FE_INVALID   0x01
#define FE_DIVBYZERO 0x04
#define FE_OVERFLOW  0x08
#define FE_UNDERFLOW 0x10
#define FE_INEXACT   0x20
#define FE_ALL_EXCEPT (FE_INVALID|FE_DIVBYZERO|FE_OVERFLOW|FE_UNDERFLOW|FE_INEXACT)

#define FE_TONEAREST  0x000
#define FE_DOWNWARD   0x400
#define FE_UPWARD     0x800
#define FE_TOWARDZERO 0xc00

typedef unsigned int fexcept_t;
/* x86_64 fenv_t: the 28-byte x87 environment (as written by `fnstenv`) followed
 * by the 4-byte MXCSR (as written by `stmxcsr`) — matching the layout openlibm's
 * nearbyint/rint expect for their inline `fldenv`/`ldmxcsr`. Must be 32 bytes:
 * fegetenv() really does fnstenv+stmxcsr, so a smaller struct would overflow. */
typedef struct {
  unsigned int __x87[7]; /* control, status, tag, ip, cs/opcode, dp, ds */
  unsigned int __mxcsr;
} fenv_t;

int feclearexcept(int excepts);
int feraiseexcept(int excepts);
int fetestexcept(int excepts);
int fegetexceptflag(fexcept_t *flagp, int excepts);
int fesetexceptflag(const fexcept_t *flagp, int excepts);
int fegetround(void);
int fesetround(int round);
int fegetenv(fenv_t *envp);
int fesetenv(const fenv_t *envp);
int feholdexcept(fenv_t *envp);
int feupdateenv(const fenv_t *envp);

#ifdef __cplusplus
}
#endif
#endif
