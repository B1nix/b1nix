#include <string.h>
#include <b1nix/mm.h>

/* Above this many bytes, hand the copy to the CPU's own string move.
 *
 * `rep movsb` carries a fixed setup cost, so for short runs the plain loop
 * below still wins; past a few hundred bytes the microcode moves far wider
 * chunks per step than eight bytes at a time (Enhanced REP MOVSB, on every
 * CPU this kernel targets) and pulls ahead by a wide margin. The sizes that
 * matter here are not small: zeroing a 4 KiB page, copying a disk block,
 * moving a user buffer in or out of a syscall.
 *
 * No CPUID gate is needed — `rep movsb`/`rep stosb` are correct on any x86,
 * only their speed varies, and the threshold is what selects for that. They
 * are string instructions, not vector ones, so the kernel's ban on SSE/AVX is
 * untouched. Not used for device memory: MMIO wants explicit accesses of a
 * known width, and driver code does not reach these below the threshold. */
#define STRING_INSN_THRESHOLD 256u

void *memcpy(void *dest, const void *src, size_t count)
{
	unsigned char *d = dest;
	const unsigned char *s = src;
#ifdef __x86_64__
	if (count >= STRING_INSN_THRESHOLD) {
		/* cld: the copy must run forward. The ABI hands every function DF
		 * clear and this kernel never sets it, so this is belt-and-braces
		 * against an interrupt frame that arrives with it set. */
		__asm__ volatile("cld; rep movsb"
		                 : "+D"(d), "+S"(s), "+c"(count)
		                 :
		                 : "memory");
		return dest;
	}
#endif

	/* Only widen the copy when both sides share an 8-byte phase: aarch64
	 * traps unaligned accesses to device memory, and the wide loop below is
	 * the one path that would issue them. */
	if (((uintptr_t)d & 7) == ((uintptr_t)s & 7)) {
		while (count > 0 && ((uintptr_t)d & 7)) {
			*d++ = *s++;
			count--;
		}
		while (count >= 8) {
			*(unsigned long long *)d = *(const unsigned long long *)s;
			d += 8;
			s += 8;
			count -= 8;
		}
	}
	while (count > 0) {
		*d++ = *s++;
		count--;
	}
	return dest;
}

void *memset(void *dest, int value, size_t count)
{
	unsigned char *d = dest;
	unsigned char v = (unsigned char)value;
	unsigned long long v64 = (unsigned long long)v * 0x0101010101010101ULL;

#ifdef __x86_64__
	if (count >= STRING_INSN_THRESHOLD) {
		__asm__ volatile("cld; rep stosb"
		                 : "+D"(d), "+c"(count)
		                 : "a"(v)
		                 : "memory");
		return dest;
	}
#endif

	while (count > 0 && ((uintptr_t)d & 7)) {
		*d++ = v;
		count--;
	}
	while (count >= 8) {
		*(unsigned long long *)d = v64;
		d += 8;
		count -= 8;
	}
	while (count > 0) {
		*d++ = v;
		count--;
	}
	return dest;
}

void *memmove(void *dest, const void *src, size_t count)
{
	unsigned char *d = dest;
	const unsigned char *s = src;

	if (d < s) {
		while (count >= 8) {
			*(unsigned long long *)d = *(const unsigned long long *)s;
			d += 8;
			s += 8;
			count -= 8;
		}
		while (count > 0) {
			*d++ = *s++;
			count--;
		}
	} else if (d > s) {
		d += count;
		s += count;
		while (count >= 8) {
			d -= 8;
			s -= 8;
			*(unsigned long long *)d = *(const unsigned long long *)s;
			count -= 8;
		}
		while (count > 0) {
			*--d = *--s;
			count--;
		}
	}

	return dest;
}

int memcmp(const void *ptr1, const void *ptr2, size_t count)
{
	const unsigned char *p1 = ptr1;
	const unsigned char *p2 = ptr2;
	for (size_t i = 0; i < count; i++) {
		if (p1[i] < p2[i]) return -1;
		if (p1[i] > p2[i]) return 1;
	}
	return 0;
}

int strcmp(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}

	return (unsigned char)*left - (unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t n)
{
	while (n > 0 && *left != '\0' && *left == *right) {
		left++;
		right++;
		n--;
	}

	if (n == 0) {
		return 0;
	}

	return (unsigned char)*left - (unsigned char)*right;
}

size_t strlen(const char *text)
{
	size_t length = 0;

	while (text[length] != '\0') {
		length++;
	}

	return length;
}

char *strcpy(char *dest, const char *src)
{
	char *d = dest;
	while ((*d++ = *src++) != '\0');
	return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
	size_t i;
	for (i = 0; i < n && src[i] != '\0'; i++) {
		dest[i] = src[i];
	}
	for (; i < n; i++) {
		dest[i] = '\0';
	}
	return dest;
}

char *strcat(char *dest, const char *src)
{
	char *d = dest;
	while (*d) d++;
	while ((*d++ = *src++) != '\0');
	return dest;
}

char *strncat(char *dest, const char *src, size_t n)
{
	char *d = dest;
	while (*d) d++;
	size_t i;
	for (i = 0; i < n && src[i] != '\0'; i++) {
		d[i] = src[i];
	}
	d[i] = '\0';
	return dest;
}

char *strchr(const char *s, int c)
{
	while (*s != '\0') {
		if (*s == (char)c) return (char *)s;
		s++;
	}
	return (c == '\0') ? (char *)s : 0;
}

char *strrchr(const char *s, int c)
{
	const char *last = 0;
	while (*s != '\0') {
		if (*s == (char)c) last = s;
		s++;
	}
	if (c == '\0') return (char *)s;
	return (char *)last;
}

char *strdup(const char *s)
{
	size_t len = strlen(s) + 1;
	char *new_s = kmalloc(len);
	if (new_s) {
		memcpy(new_s, s, len);
	}
	return new_s;
}

char *strtok(char *str, const char *delim)
{
	static char *last = 0;
	
	if (str) last = str;
	if (!last || *last == '\0') return 0;
	
	/* Skip leading delimiters */
	while (*last && strchr(delim, *last)) last++;
	if (*last == '\0') return 0;
	
	char *token_start = last;
	
	/* Find end of token */
	while (*last && !strchr(delim, *last)) last++;
	
	if (*last) {
		*last = '\0';
		last++;
	} else {
		last = 0;
	}
	
	return token_start;
}

char *strstr(const char *haystack, const char *needle)
{
	if (!haystack || !needle) return 0;
	if (*needle == '\0') return (char *)haystack;

	size_t needle_len = strlen(needle);
	while (*haystack) {
		if (strncmp(haystack, needle, needle_len) == 0)
			return (char *)haystack;
		haystack++;
	}
	return 0;
}

size_t strnlen(const char *s, size_t maxlen)
{
	usize n = 0;
	while (n < maxlen && s[n])
		n++;
	return n;
}

/* Append, never writing past `size` bytes total including the terminator.
 * Returns the length the result would have had, so a caller can detect
 * truncation — which is what distinguishes it from strncat. */
size_t strlcat(char *dst, const char *src, size_t size)
{
	size_t dlen = strnlen(dst, size);
	size_t slen = strlen(src);
	size_t i;

	if (dlen == size)
		return size + slen;
	for (i = 0; dlen + i < size - 1 && src[i]; i++)
		dst[dlen + i] = src[i];
	dst[dlen + i] = '\0';
	return dlen + slen;
}
