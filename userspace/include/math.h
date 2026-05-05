#ifndef B1NIX_U_MATH_H
#define B1NIX_U_MATH_H

double strtod(const char *nptr, char **endptr);
double ldexp(double x, int exp);
double frexp(double x, int *exp);
long double ldexpl(long double x, int exp);
float strtof(const char *nptr, char **endptr);
long double strtold(const char *nptr, char **endptr);

#endif
