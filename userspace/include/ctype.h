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

static inline int isalnum(int c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
}

static inline int isalpha(int c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}

static inline int iscntrl(int c) {
    return (c >= 0 && c < 32) || (c == 127);
}

static inline int isdigit(int c) {
    return (c >= '0' && c <= '9');
}

static inline int isgraph(int c) {
    return (c >= 33 && c <= 126);
}

static inline int islower(int c) {
    return (c >= 'a' && c <= 'z');
}

static inline int isprint(int c) {
    return (c >= 32 && c <= 126);
}

static inline int ispunct(int c) {
    return isgraph(c) && !isalnum(c);
}

static inline int isspace(int c) {
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f');
}

static inline int isupper(int c) {
    return (c >= 'A' && c <= 'Z');
}

static inline int isxdigit(int c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

#ifdef __cplusplus
extern "C" {
#endif
int isblank(int c);
#ifdef __cplusplus
}
#endif

static inline int isascii(int c) {
    return (c >= 0 && c <= 127);
}

static inline int toascii(int c) {
    return (c & 0x7f);
}

static inline int tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
}

static inline int toupper(int c) {
    return (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
}

#endif
