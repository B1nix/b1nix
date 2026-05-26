#ifndef B1NIX_U_MATH_H
#define B1NIX_U_MATH_H

#define FP_NAN          0
#define FP_INFINITE     1
#define FP_ZERO         2
#define FP_SUBNORMAL    3
#define FP_NORMAL       4

#ifdef __cplusplus
extern "C" {
#endif

double strtod(const char *nptr, char **endptr);
double ldexp(double x, int exp);
double frexp(double x, int *exp);
long double ldexpl(long double x, int exp);
float strtof(const char *nptr, char **endptr);
long double strtold(const char *nptr, char **endptr);

#ifdef __cplusplus
}
#endif

static inline double acos(double x) { return __builtin_acos(x); }
static inline double asin(double x) { return __builtin_asin(x); }
static inline double atan(double x) { return __builtin_atan(x); }
static inline double atan2(double y, double x) { return __builtin_atan2(y, x); }
static inline double ceil(double x) { return __builtin_ceil(x); }
static inline double cos(double x) { return __builtin_cos(x); }
static inline double cosh(double x) { return __builtin_cosh(x); }
static inline double exp(double x) { return __builtin_exp(x); }
static inline double fabs(double x) { return __builtin_fabs(x); }
static inline double floor(double x) { return __builtin_floor(x); }
static inline double fmod(double x, double y) { return __builtin_fmod(x, y); }
static inline double log(double x) { return __builtin_log(x); }
static inline double log10(double x) { return __builtin_log10(x); }
static inline double modf(double x, double *iptr) { return __builtin_modf(x, iptr); }
static inline double pow(double x, double y) { return __builtin_pow(x, y); }
static inline double sin(double x) { return __builtin_sin(x); }
static inline double sinh(double x) { return __builtin_sinh(x); }
static inline double sqrt(double x) { return __builtin_sqrt(x); }
static inline double tan(double x) { return __builtin_tan(x); }
static inline double tanh(double x) { return __builtin_tanh(x); }

#endif
