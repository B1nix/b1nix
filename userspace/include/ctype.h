#ifndef B1NIX_U_CTYPE_H
#define B1NIX_U_CTYPE_H

#define _U      01
#define _L      02
#define _N      04
#define _S      010
#define _P      020
#define _C      040
#define _X      0100
#define _B      0200

/* newlib-compatible character classification table. libstdc++'s generic
 * (newlib) ctype config reads classic_table() as `_ctype_ + 1`, so index 0 is
 * the EOF slot and indices 1..256 classify chars 0..255 using the _U/_L/_N/...
 * bit masks above. Defined in libc/ctype.c. Required for the native GCC port's
 * C++ runtime. */
#ifdef __cplusplus
extern "C" {
#endif
extern const char _ctype_[];
#ifdef __cplusplus
}
#endif

#ifndef __cplusplus
#define isalnum(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || ((c) >= '0' && (c) <= '9'))
#define isalpha(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z'))
#define iscntrl(c) (((c) >= 0 && (c) < 32) || (c) == 127)
#define isdigit(c) ((c) >= '0' && (c) <= '9')
#define isgraph(c) ((c) >= 33 && (c) <= 126)
#define islower(c) ((c) >= 'a' && (c) <= 'z')
#define isprint(c) ((c) >= 32 && (c) <= 126)
#define ispunct(c) (isgraph(c) && !isalnum(c))
#define isspace(c) ((c) == ' ' || (c) == '\t' || (c) == '\n' || (c) == '\r' || (c) == '\v' || (c) == '\f')
#define isupper(c) ((c) >= 'A' && (c) <= 'Z')
#define isxdigit(c) (((c) >= '0' && (c) <= '9') || ((c) >= 'a' && (c) <= 'f') || ((c) >= 'A' && (c) <= 'F'))

#define isascii(c) ((c) >= 0 && (c) <= 127)
#define toascii(c) ((c) & 0x7f)

#define tolower(c) (((c) >= 'A' && (c) <= 'Z') ? ((c) - 'A' + 'a') : (c))
#define toupper(c) (((c) >= 'a' && (c) <= 'z') ? ((c) - 'a' + 'A') : (c))
#else
extern "C" {
int isalnum(int c);
int isalpha(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);
int isascii(int c);
int toascii(int c);
int tolower(int c);
int toupper(int c);
}
#endif

#ifdef __cplusplus
extern "C" {
#endif
int isblank(int c);
#ifdef __cplusplus
}
#endif

/* C-locale-only xlocale (_l) variants — ignore the locale, delegate. Used by
 * libc++'s locale layer. */
#ifndef B1NIX_LOCALE_T_DEFINED
#define B1NIX_LOCALE_T_DEFINED
typedef void *locale_t;
#endif
static inline int isalnum_l(int c, locale_t l) { (void)l; return isalnum(c); }
static inline int isalpha_l(int c, locale_t l) { (void)l; return isalpha(c); }
static inline int isblank_l(int c, locale_t l) { (void)l; return isblank(c); }
static inline int iscntrl_l(int c, locale_t l) { (void)l; return iscntrl(c); }
static inline int isdigit_l(int c, locale_t l) { (void)l; return isdigit(c); }
static inline int isgraph_l(int c, locale_t l) { (void)l; return isgraph(c); }
static inline int islower_l(int c, locale_t l) { (void)l; return islower(c); }
static inline int isprint_l(int c, locale_t l) { (void)l; return isprint(c); }
static inline int ispunct_l(int c, locale_t l) { (void)l; return ispunct(c); }
static inline int isspace_l(int c, locale_t l) { (void)l; return isspace(c); }
static inline int isupper_l(int c, locale_t l) { (void)l; return isupper(c); }
static inline int isxdigit_l(int c, locale_t l) { (void)l; return isxdigit(c); }
static inline int tolower_l(int c, locale_t l) { (void)l; return tolower(c); }
static inline int toupper_l(int c, locale_t l) { (void)l; return toupper(c); }

#endif
