#include <fnmatch.h>
#include <stddef.h>

static int strlen(const char *s) {
    int len = 0;
    while (*s++) len++;
    return len;
}

static int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static void *memcpy(void *dest, const void *src, unsigned int n) {
    char *d = dest;
    const char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}

int fnmatch(const char *pattern, const char *string, int flags) {
    const char *p = pattern;
    const char *s = string;

    while (*p) {
        if (*p == '*') {
            while (*p == '*') {
                p++;
            }

            if ((flags & FNM_PERIOD) && *s == '.') {
                if (s == string || ((flags & FNM_PATHNAME) && s > string && *(s - 1) == '/')) {
                    return FNM_NOMATCH;
                }
            }

            if (!*p) {
                if (flags & FNM_PATHNAME) {
                    while (*s) {
                        if (*s == '/') {
                            return FNM_NOMATCH;
                        }
                        s++;
                    }
                    return 0;
                }
                return 0;
            }

            while (*s) {
                if ((flags & FNM_PATHNAME) && *s == '/') {
                    break;
                }
                if (fnmatch(p, s, flags) == 0) {
                    return 0;
                }
                s++;
            }
            return FNM_NOMATCH;
        } else if (*p == '?') {
            if (!*s) {
                return FNM_NOMATCH;
            }
            if ((flags & FNM_PATHNAME) && *s == '/') {
                return FNM_NOMATCH;
            }
            if ((flags & FNM_PERIOD) && *s == '.') {
                if (s == string || ((flags & FNM_PATHNAME) && s > string && *(s - 1) == '/')) {
                    return FNM_NOMATCH;
                }
            }
            p++;
            s++;
        } else if (*p == '\\' && !(flags & FNM_NOESCAPE)) {
            p++;
            if (!*p) {
                if (*s != '\\') return FNM_NOMATCH;
            } else {
                if (*p != *s) {
                    return FNM_NOMATCH;
                }
            }
            p++;
            s++;
        } else if (*p == '[') {
            if (!*s) {
                return FNM_NOMATCH;
            }
            if ((flags & FNM_PATHNAME) && *s == '/') {
                return FNM_NOMATCH;
            }
            if ((flags & FNM_PERIOD) && *s == '.') {
                if (s == string || ((flags & FNM_PATHNAME) && s > string && *(s - 1) == '/')) {
                    return FNM_NOMATCH;
                }
            }

            p++;
            int negate = 0;
            if (*p == '!' || *p == '^') {
                negate = 1;
                p++;
            }

            int match = 0;
            char c = *s;
            int first = 1;
            while (*p && (*p != ']' || first)) {
                first = 0;
                char start = *p;
                if (start == '\\' && !(flags & FNM_NOESCAPE)) {
                    p++;
                    start = *p;
                }
                p++;
                if (*p == '-' && p[1] != '\0' && p[1] != ']') {
                    p++;
                    char end = *p;
                    if (end == '\\' && !(flags & FNM_NOESCAPE)) {
                        p++;
                        end = *p;
                    }
                    p++;
                    if (c >= start && c <= end) {
                        match = 1;
                    }
                } else {
                    if (c == start) {
                        match = 1;
                    }
                }
            }

            if (*p != ']') {
                return FNM_NOMATCH;
            }
            p++;

            if (match == negate) {
                return FNM_NOMATCH;
            }
            s++;
        } else {
            if (*p != *s) {
                return FNM_NOMATCH;
            }
            p++;
            s++;
        }
    }

    return *s ? FNM_NOMATCH : 0;
}
