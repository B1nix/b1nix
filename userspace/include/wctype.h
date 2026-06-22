#ifndef B1NIX_U_WCTYPE_H
#define B1NIX_U_WCTYPE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef B1NIX_WINT_T_DEFINED
#define B1NIX_WINT_T_DEFINED
typedef int wint_t;
#endif

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

typedef unsigned long wctype_t;
typedef const int *wctrans_t;

wctype_t wctype(const char *name);
int iswctype(wint_t wc, wctype_t desc);

wctrans_t wctrans(const char *name);
wint_t towctrans(wint_t wc, wctrans_t desc);

int iswalnum(wint_t wc);
int iswalpha(wint_t wc);
int iswcntrl(wint_t wc);
int iswdigit(wint_t wc);
int iswgraph(wint_t wc);
int iswlower(wint_t wc);
int iswprint(wint_t wc);
int iswpunct(wint_t wc);
int iswspace(wint_t wc);
int iswupper(wint_t wc);
int iswxdigit(wint_t wc);
int iswblank(wint_t wc);

wint_t towlower(wint_t wc);
wint_t towupper(wint_t wc);

#ifdef __cplusplus
}
#endif

#endif
