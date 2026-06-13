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

#define isnan(x)        __builtin_isnan(x)
#define isinf(x)        __builtin_isinf(x)
#define isfinite(x)     __builtin_isfinite(x)
#define signbit(x)      __builtin_signbit(x)

#ifdef __cplusplus
extern "C" {
#endif

/* Provided by libm.a (openlibm). These MUST be plain prototypes, never
 * `static inline { return __builtin_f(...); }`: the builtin lowers to a call
 * to the same symbol, so an inline wrapper is infinite self-recursion that the
 * optimizer turns into a `jmp .` hang (it only ever "worked" when the args were
 * compile-time constant and folded). See docs/m51-plan.md. */
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

double strtod(const char *nptr, char **endptr);
double ldexp(double x, int exp);
double frexp(double x, int *exp);
long double ldexpl(long double x, int exp);
float strtof(const char *nptr, char **endptr);
long double strtold(const char *nptr, char **endptr);

#ifdef __cplusplus
}
#endif

#endif
