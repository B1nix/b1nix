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

/* C-locale-only xlocale (_l) variants for libc++ — ignore the locale. */
#ifndef B1NIX_LOCALE_T_DEFINED
#define B1NIX_LOCALE_T_DEFINED
typedef void *locale_t;
#endif
static inline int iswalnum_l(wint_t c, locale_t l) { (void)l; return iswalnum(c); }
static inline int iswalpha_l(wint_t c, locale_t l) { (void)l; return iswalpha(c); }
static inline int iswblank_l(wint_t c, locale_t l) { (void)l; return iswblank(c); }
static inline int iswcntrl_l(wint_t c, locale_t l) { (void)l; return iswcntrl(c); }
static inline int iswdigit_l(wint_t c, locale_t l) { (void)l; return iswdigit(c); }
static inline int iswgraph_l(wint_t c, locale_t l) { (void)l; return iswgraph(c); }
static inline int iswlower_l(wint_t c, locale_t l) { (void)l; return iswlower(c); }
static inline int iswprint_l(wint_t c, locale_t l) { (void)l; return iswprint(c); }
static inline int iswpunct_l(wint_t c, locale_t l) { (void)l; return iswpunct(c); }
static inline int iswspace_l(wint_t c, locale_t l) { (void)l; return iswspace(c); }
static inline int iswupper_l(wint_t c, locale_t l) { (void)l; return iswupper(c); }
static inline int iswxdigit_l(wint_t c, locale_t l) { (void)l; return iswxdigit(c); }
static inline int iswctype_l(wint_t c, wctype_t t, locale_t l) { (void)l; return iswctype(c, t); }
static inline wint_t towlower_l(wint_t c, locale_t l) { (void)l; return towlower(c); }
static inline wint_t towupper_l(wint_t c, locale_t l) { (void)l; return towupper(c); }
static inline wint_t towctrans_l(wint_t c, wctrans_t t, locale_t l) { (void)l; return towctrans(c, t); }
static inline wctype_t wctype_l(const char *p, locale_t l) { (void)l; return wctype(p); }
static inline wctrans_t wctrans_l(const char *p, locale_t l) { (void)l; return wctrans(p); }

#ifdef __cplusplus
}
#endif

#endif
