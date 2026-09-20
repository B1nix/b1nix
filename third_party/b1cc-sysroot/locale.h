#ifndef B1NIX_U_LOCALE_H
#define B1NIX_U_LOCALE_H

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5
#define LC_MESSAGES 6

struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    /* C99 international-monetary fields (libc++ reads them; C-locale = CHAR_MAX). */
    char int_p_cs_precedes;
    char int_p_sep_by_space;
    char int_n_cs_precedes;
    char int_n_sep_by_space;
    char int_p_sign_posn;
    char int_n_sign_posn;
};

#ifdef __cplusplus
extern "C" {
#endif

char *setlocale(int category, const char *locale);
struct lconv *localeconv(void);

/* xlocale / per-call locale API (added for the Chromium port, M60-62). b1nix is
 * C-locale ONLY, so locale_t is an opaque non-NULL handle and the *_l functions
 * ignore the locale (they behave as the C locale, which is all b1nix has). This
 * is correct b1nix behaviour, not a fake — there are no other locales. */
#ifndef B1NIX_LOCALE_T_DEFINED
#define B1NIX_LOCALE_T_DEFINED
typedef void *locale_t;
#endif

/* newlocale category masks. */
#define LC_CTYPE_MASK    (1 << LC_CTYPE)
#define LC_NUMERIC_MASK  (1 << LC_NUMERIC)
#define LC_TIME_MASK     (1 << LC_TIME)
#define LC_COLLATE_MASK  (1 << LC_COLLATE)
#define LC_MONETARY_MASK (1 << LC_MONETARY)
#define LC_MESSAGES_MASK (1 << LC_MESSAGES)
#define LC_ALL_MASK      (~0)
#define LC_GLOBAL_LOCALE ((locale_t)-1)

locale_t newlocale(int category_mask, const char *locale, locale_t base);
locale_t duplocale(locale_t locobj);
void     freelocale(locale_t locobj);
locale_t uselocale(locale_t newloc);

#ifdef __cplusplus
}
#endif

#endif
