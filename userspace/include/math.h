#ifndef B1NIX_U_MATH_H
#define B1NIX_U_MATH_H

#define FP_NAN          0
#define FP_INFINITE     1
#define FP_ZERO         2
#define FP_SUBNORMAL    3
#define FP_NORMAL       4

#define M_E             2.7182818284590452354
#define M_LOG2E         1.4426950408889634074
#define M_LOG10E        0.43429448190325182765
#define M_LN2           0.69314718055994530942
#define M_LN10          2.30258509299404568402
#define M_PI            3.14159265358979323846
#define M_PI_2          1.57079632679489661923
#define M_PI_4          0.78539816339744830962
#define M_1_PI          0.31830988618379067154
#define M_2_PI          0.63661977236758134308
#define M_2_SQRTPI      1.12837916709551257390
#define M_SQRT2         1.41421356237309504880
#define M_SQRT1_2       0.70710678118654752440

#define HUGE_VAL        __builtin_huge_val()
#define HUGE_VALF       __builtin_huge_valf()
#define INFINITY        __builtin_inff()
#define NAN             __builtin_nanf("")

/* Classification helpers are function-like macros in C only. In C++ they must
 * NOT be macros (std::isfinite(x) would expand to std::__builtin_isfinite(x))
 * and must NOT be redefined here either: libstdc++'s own <math.h>/<cmath>
 * wrappers provide both std::isfinite and a global ::isfinite (when
 * _GLIBCXX_USE_C99_MATH is enabled), so defining our own would conflict. */
#ifndef __cplusplus
#define isnan(x)        __builtin_isnan(x)
#define isinf(x)        __builtin_isinf(x)
#define isfinite(x)     __builtin_isfinite(x)
#define signbit(x)      __builtin_signbit(x)
#define isnormal(x)     __builtin_isnormal(x)
#define isgreater(x,y)  __builtin_isgreater(x,y)
#define isless(x,y)     __builtin_isless(x,y)
#define fpclassify(x)   __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Provided by libm.a (openlibm). These MUST be plain prototypes, never
 * `static inline { return __builtin_f(...); }`: the builtin lowers to a call
 * to the same symbol, so an inline wrapper is infinite self-recursion that the
 * optimizer turns into a `jmp .` hang (it only ever "worked" when the args were
 * compile-time constant and folded). */
double acos(double);
double asin(double);
double atan(double);
double atan2(double, double);
double cos(double);
double cosh(double);
double exp(double);
double exp2(double);
double expm1(double);
double fabs(double);
double floor(double);
double ceil(double);
double trunc(double);
double round(double);
double rint(double);
double nearbyint(double);
double fmod(double, double);
double remainder(double, double);
double log(double);
double log2(double);
double log10(double);
double log1p(double);
double modf(double, double *);
double pow(double, double);
double sqrt(double);
double cbrt(double);
double hypot(double, double);
double sin(double);
double sinh(double);
double tan(double);
double tanh(double);
double copysign(double, double);
double scalbn(double, int);
double fmax(double, double);
double fmin(double, double);
double fdim(double, double);
double nextafter(double, double);

float acosf(float);
float asinf(float);
float atanf(float);
float atan2f(float, float);
float cosf(float);
float expf(float);
float fabsf(float);
float floorf(float);
float ceilf(float);
float truncf(float);
float roundf(float);
float rintf(float);
float fmodf(float, float);
float logf(float);
float log2f(float);
float log10f(float);
float powf(float, float);
float sqrtf(float);
float sinf(float);
float tanf(float);
float copysignf(float, float);
float scalbnf(float, int);
float hypotf(float, float);
float fmaxf(float, float);
float fminf(float, float);

double strtod(const char *nptr, char **endptr);
double ldexp(double x, int exp);
double frexp(double x, int *exp);
long double ldexpl(long double x, int exp);
float strtof(const char *nptr, char **endptr);
long double strtold(const char *nptr, char **endptr);




/* round-to-nearest-integer (in libm) */
long lrint(double);
long lrintf(float);
long long llrint(double);
long long llrintf(float);


/* C99 math additions for ports (Mesa/NIR). Backed by openlibm. */
float exp2f(float);
float fmaf(float, float, float);
double fma(double, double, double);
float ldexpf(float, int);


/* Broad C99 math decls (openlibm-backed) for Mesa/NIR. */
float frexpf(float, int *);
float modff(float, float *);
float nearbyintf(float);
float nextafterf(float, float);
float cbrtf(float);
float expm1f(float);
float log1pf(float);
double asinh(double);
float asinhf(float);
double acosh(double);
float acoshf(float);
double atanh(double);
float atanhf(float);
float remainderf(float, float);
long lround(double);

long lroundf(float);

long long llround(double);

long long llroundf(float);

double nexttoward(double, long double);
float nexttowardf(float, long double);
long double nexttowardl(long double, long double);

/* C99 evaluation-format types (used by libstdc++ <cmath> when
 * _GLIBCXX_USE_C99_MATH_TR1 is on). x86 evaluates in the wider format, but the
 * standard only requires these to be at least as wide as their base type. */
typedef double double_t;
typedef float float_t;

/* Full C99 completion (openlibm-backed). libstdc++'s <cmath> re-declares the
 * entire C99 <math.h> in namespace std when _GLIBCXX_USE_C99_MATH_TR1 is
 * enabled, so every prototype below must exist even if a given port only uses a
 * few. The long-double variants resolve against openlibm; an unused one never
 * needs to link. Required by C++ ports such as libjxl. */
double erf(double);
float erff(float);
long double erfl(long double);
double erfc(double);
float erfcf(float);
long double erfcl(long double);
double lgamma(double);
float lgammaf(float);
long double lgammal(long double);
double tgamma(double);
float tgammaf(float);
long double tgammal(long double);
int ilogb(double);
int ilogbf(float);
int ilogbl(long double);
double logb(double);
float logbf(float);
long double logbl(long double);
double scalbln(double, long);
float scalblnf(float, long);
long double scalblnl(long double, long);
double remquo(double, double, int *);
float remquof(float, float, int *);
long double remquol(long double, long double, int *);
double nan(const char *);
float nanf(const char *);
long double nanl(const char *);
float fdimf(float, float);

/* Long-double (l) variants of the base C99 set. */
long double acosl(long double);
long double asinl(long double);
long double atanl(long double);
long double atan2l(long double, long double);
long double cosl(long double);
long double coshl(long double);
long double expl(long double);
long double exp2l(long double);
long double expm1l(long double);
long double fabsl(long double);
long double floorl(long double);
long double ceill(long double);
long double truncl(long double);
long double roundl(long double);
long double rintl(long double);
long double nearbyintl(long double);
long double fmodl(long double, long double);
long double remainderl(long double, long double);
long double logl(long double);
long double log2l(long double);
long double log10l(long double);
long double log1pl(long double);
long double modfl(long double, long double *);
long double powl(long double, long double);
long double sqrtl(long double);
long double cbrtl(long double);
long double hypotl(long double, long double);
long double sinl(long double);
long double sinhl(long double);
long double tanl(long double);
long double tanhl(long double);
long double copysignl(long double, long double);
long double scalbnl(long double, int);
long double fmaxl(long double, long double);
long double fminl(long double, long double);
long double fdiml(long double, long double);
long double nextafterl(long double, long double);
long double frexpl(long double, int *);
long double fmal(long double, long double, long double);
long double acoshl(long double);
long double asinhl(long double);
long double atanhl(long double);
long lrintl(long double);
long long llrintl(long double);
long lroundl(long double);
long long llroundl(long double);

#ifdef __cplusplus
}
#endif

#endif
