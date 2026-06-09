#include <string.h>
#include <stdlib.h>
#include "syscall.h"
#include <errno.h>

void *memcpy(void *dest, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dest;
	const unsigned char *s = (const unsigned char *)src;
	for (size_t i = 0; i < n; i++) d[i] = s[i];
	return dest;
}

void *memset(void *dest, int v, size_t n)
{
	unsigned char *d = (unsigned char *)dest;
	for (size_t i = 0; i < n; i++) d[i] = (unsigned char)v;
	return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dest;
	const unsigned char *s = (const unsigned char *)src;
	if (d < s) {
		for (size_t i = 0; i < n; i++) d[i] = s[i];
	} else {
		for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
	}
	return dest;
}

int memcmp(const void *p1, const void *p2, size_t n)
{
	const unsigned char *a = (const unsigned char *)p1;
	const unsigned char *b = (const unsigned char *)p2;
	for (size_t i = 0; i < n; i++) {
		if (a[i] != b[i]) return (int)a[i] - (int)b[i];
	}
	return 0;
}

size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n]) n++;
	return n;
}

size_t strnlen(const char *s, size_t maxlen)
{
	size_t n = 0;
	while (n < maxlen && s[n]) n++;
	return n;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strcoll(const char *a, const char *b)
{
	return strcmp(a, b);
}

int strncmp(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
		if (a[i] == '\0') return 0;
	}
	return 0;
}

char *strcpy(char *dest, const char *src)
{
	char *d = dest;
	while ((*d++ = *src++));
	return dest;
}

char *stpcpy(char *dest, const char *src)
{
	while ((*dest = *src)) {
		dest++;
		src++;
	}
	return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
	size_t i;
	for (i = 0; i < n && src[i]; i++) dest[i] = src[i];
	for (; i < n; i++) dest[i] = '\0';
	return dest;
}

char *stpncpy(char *dest, const char *src, size_t n)
{
	size_t i;
	for (i = 0; i < n && src[i]; i++) dest[i] = src[i];
	char *end = dest + i;
	for (; i < n; i++) dest[i] = '\0';
	return end;
}

char *strchr(const char *s, int c)
{
	while (*s) {
		if (*s == (char)c) return (char *)s;
		s++;
	}
	return (c == '\0') ? (char *)s : 0;
}

char *strstr(const char *haystack, const char *needle)
{
	if (!*needle) return (char *)haystack;
	size_t nl = strlen(needle);
	while (*haystack) {
		if (strncmp(haystack, needle, nl) == 0) return (char *)haystack;
		haystack++;
	}
	return 0;
}

char *strdup(const char *s)
{
	size_t len = strlen(s) + 1;
	char *p = malloc(len);
	if (p) memcpy(p, s, len);
	return p;
}

char *strndup(const char *s, size_t n)
{
	size_t len = 0;
	while (len < n && s[len]) len++;
	char *p = malloc(len + 1);
	if (p) {
		memcpy(p, s, len);
		p[len] = '\0';
	}
	return p;
}

char *strcat(char *dest, const char *src)
{
	char *d = dest;
	while (*d) d++;
	while ((*d++ = *src++));
	return dest;
}

char *strncat(char *dest, const char *src, size_t n)
{
	char *d = dest;
	while (*d) d++;
	size_t i;
	for (i = 0; i < n && src[i]; i++) d[i] = src[i];
	d[i] = '\0';
	return dest;
}

char *strrchr(const char *s, int c)
{
	const char *last = NULL;
	while (*s) {
		if (*s == (char)c) last = s;
		s++;
	}
	if (c == '\0') return (char *)s;
	return (char *)last;
}

char *strerror(int errnum)
{
	switch (errnum) {
		case 0: return "Success";
		case EPERM: return "Operation not permitted";
		case ENOENT: return "No such file or directory";
		case ESRCH: return "No such process";
		case EINTR: return "Interrupted system call";
		case EIO: return "I/O error";
		case ENXIO: return "No such device or address";
		case E2BIG: return "Argument list too long";
		case ENOEXEC: return "Exec format error";
		case EBADF: return "Bad file number";
		case ECHILD: return "No child processes";
		case EAGAIN: return "Try again";
		case ENOMEM: return "Out of memory";
		case EACCES: return "Permission denied";
		case EFAULT: return "Bad address";
		case ENOTBLK: return "Block device required";
		case EBUSY: return "Device or resource busy";
		case EEXIST: return "File exists";
		case EXDEV: return "Cross-device link";
		case ENODEV: return "No such device";
		case ENOTDIR: return "Not a directory";
		case EISDIR: return "Is a directory";
		case EINVAL: return "Invalid argument";
		case ENFILE: return "File table overflow";
		case EMFILE: return "Too many open files";
		case ENOTTY: return "Not a typewriter";
		case ETXTBSY: return "Text file busy";
		case EFBIG: return "File too large";
		case ENOSPC: return "No space left on device";
		case ESPIPE: return "Illegal seek";
		case EROFS: return "Read-only file system";
		case EMLINK: return "Too many links";
		case EPIPE: return "Broken pipe";
		case EDOM: return "Math argument out of domain of func";
		case ERANGE: return "Math result not representable";
		default: return "Unknown error";
	}
}

char *strpbrk(const char *s, const char *accept)
{
	while (*s) {
		const char *a = accept;
		while (*a) {
			if (*a == *s) return (char *)s;
			a++;
		}
		s++;
	}
	return NULL;
}

static inline char to_lower(char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
	return c;
}

int strcasecmp(const char *a, const char *b) {
	while (*a && to_lower(*a) == to_lower(*b)) {
		a++;
		b++;
	}
	return (int)(unsigned char)to_lower(*a) - (int)(unsigned char)to_lower(*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
	for (size_t i = 0; i < n; i++) {
		char la = to_lower(a[i]);
		char lb = to_lower(b[i]);
		if (la != lb) return (int)(unsigned char)la - (int)(unsigned char)lb;
		if (la == '\0') return 0;
	}
	return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return NULL;
}

char *strtok(char *str, const char *delim) {
    static char *last;
    if (str) last = str;
    if (!last || *last == '\0') return NULL;
    while (*last) {
        const char *d = delim;
        while (*d) {
            if (*last == *d) break;
            d++;
        }
        if (!*d) break;
        last++;
    }
    if (*last == '\0') return NULL;
    char *start = last;
    while (*last) {
        const char *d = delim;
        while (*d) {
            if (*last == *d) {
                *last = '\0';
                last++;
                return start;
            }
            d++;
        }
        last++;
    }
    return start;
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    if (!saveptr) return NULL;
    char *s = str ? str : *saveptr;
    if (!s || *s == '\0') return NULL;
    while (*s && strchr(delim, *s)) s++;
    if (*s == '\0') {
        *saveptr = NULL;
        return NULL;
    }
    char *start = s;
    while (*s && !strchr(delim, *s)) s++;
    if (*s) {
        *s++ = '\0';
        *saveptr = s;
    } else {
        *saveptr = NULL;
    }
    return start;
}

char *strsep(char **stringp, const char *delim) {
    if (!stringp || !*stringp) {
        return NULL;
    }

    char *start = *stringp;
    char *p = start;
    while (*p) {
        if (strchr(delim, *p)) {
            *p = '\0';
            *stringp = p + 1;
            return start;
        }
        p++;
    }
    *stringp = NULL;
    return start;
}

char *strcasestr(const char *haystack, const char *needle) {
  if (!*needle)
    return (char *)haystack;
  for (; *haystack; haystack++) {
    const char *h = haystack;
    const char *n = needle;
    while (*h && *n && to_lower(*h) == to_lower(*n)) {
      h++;
      n++;
    }
    if (!*n)
      return (char *)haystack;
  }
  return NULL;
}

size_t strxfrm(char *dest, const char *src, size_t n) {
    size_t len = strlen(src);
    if (n > len) {
        strcpy(dest, src);
    } else if (n > 0) {
        strncpy(dest, src, n - 1);
        dest[n - 1] = '\0';
    }
    return len;
}

size_t strcspn(const char *s, const char *reject) {
    const char *p = s;
    while (*p) {
        const char *r = reject;
        while (*r) {
            if (*p == *r) return p - s;
            r++;
        }
        p++;
    }
    return p - s;
}

size_t strspn(const char *s, const char *accept) {
    const char *p = s;
    while (*p) {
        const char *a = accept;
        while (*a) {
            if (*p == *a) break;
            a++;
        }
        if (!*a) return p - s;
        p++;
    }
    return p - s;
}

char *strchrnul(const char *s, int c) {
    while (*s && *s != (char)c) {
        s++;
    }
    return (char *)s;
}

#include <wchar.h>

wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t n) {
	for (size_t i = 0; i < n; i++) {
		dest[i] = src[i];
	}
	return dest;
}

wchar_t *wmemmove(wchar_t *dest, const wchar_t *src, size_t n) {
	if (dest < src) {
		for (size_t i = 0; i < n; i++) dest[i] = src[i];
	} else {
		for (size_t i = n; i > 0; i--) dest[i - 1] = src[i - 1];
	}
	return dest;
}

wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n) {
	for (size_t i = 0; i < n; i++) {
		s[i] = c;
	}
	return s;
}

size_t wcslen(const wchar_t *s) {
	size_t n = 0;
	while (s[n]) n++;
	return n;
}

wchar_t *wcscat(wchar_t *dest, const wchar_t *src) {
	wchar_t *d = dest;
	while (*d) d++;
	while ((*d++ = *src++));
	return dest;
}

wchar_t *wcscpy(wchar_t *dest, const wchar_t *src) {
	wchar_t *d = dest;
	while ((*d++ = *src++));
	return dest;
}

void *mempcpy(void *dest, const void *src, size_t n) {
  return (char *)memcpy(dest, src, n) + n;
}

/* strverscmp — GNU "version" string comparison (used by `sort -V`). Embedded
 * digit runs compare by numeric value, so "file9" sorts before "file10".
 * Classic glibc state-machine implementation. */
#define VS_ISDIGIT(c) ((c) >= '0' && (c) <= '9')
int strverscmp(const char *s1, const char *s2) {
  enum { S_N = 0x0, S_I = 0x3, S_F = 0x6, S_Z = 0x9, VS_CMP = 2, VS_LEN = 3 };
  static const unsigned char next_state[] = {
      /* state    x    d    0  */
      /* S_N */ S_N, S_I, S_Z,
      /* S_I */ S_N, S_I, S_I,
      /* S_F */ S_N, S_F, S_F,
      /* S_Z */ S_N, S_F, S_Z};
  static const signed char result_type[] = {
      /* state   x/x x/d x/0 d/x d/d d/0 0/x 0/d 0/0 */
      /* S_N */ VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_LEN, VS_CMP, VS_CMP, VS_CMP,
      VS_CMP,
      /* S_I */ VS_CMP, -1, -1, +1, VS_LEN, VS_LEN, +1, VS_LEN, VS_LEN,
      /* S_F */ VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_CMP, VS_CMP,
      VS_CMP,
      /* S_Z */ VS_CMP, +1, +1, -1, VS_CMP, VS_CMP, -1, VS_CMP, VS_CMP};

  const unsigned char *p1 = (const unsigned char *)s1;
  const unsigned char *p2 = (const unsigned char *)s2;
  unsigned char c1, c2;
  int state, diff;

  if (p1 == p2)
    return 0;

  c1 = *p1++;
  c2 = *p2++;
  state = S_N + ((c1 == '0') + (VS_ISDIGIT(c1) != 0));

  while ((diff = c1 - c2) == 0) {
    if (c1 == '\0')
      return diff;
    state = next_state[state];
    c1 = *p1++;
    c2 = *p2++;
    state += (c1 == '0') + (VS_ISDIGIT(c1) != 0);
  }

  state = result_type[state * 3 + ((c2 == '0') + (VS_ISDIGIT(c2) != 0))];

  switch (state) {
  case VS_CMP:
    return diff;
  case VS_LEN:
    while (VS_ISDIGIT(*p1++))
      if (!VS_ISDIGIT(*p2++))
        return 1;
    return VS_ISDIGIT(*p2) ? -1 : diff;
  default:
    return state;
  }
}
