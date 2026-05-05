#include <stdio.h>
#include <string.h>
#include "syscall.h"

int putchar(int c)
{
	char ch = (char)c;
	syscall(SYS_WRITE, (long)&ch, 1, 1, 0);
	return c;
}

int puts(const char *s)
{
	syscall(SYS_WRITE, (long)s, (long)strlen(s), 1, 0);
	putchar('\n');
	return 0;
}

static void print_dec(unsigned long v, char *buf, int *pos)
{
	if (v >= 10) print_dec(v / 10, buf, pos);
	buf[(*pos)++] = '0' + (v % 10);
}

static void print_hex(unsigned long v, char *buf, int *pos)
{
	const char *hex = "0123456789abcdef";
	if (v >= 16) print_hex(v / 16, buf, pos);
	buf[(*pos)++] = hex[v % 16];
}

int snprintf(char *str, size_t size, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	int pos = 0;
	for (int i = 0; fmt[i] && pos < (int)size - 1; i++) {
		if (fmt[i] != '%') {
			str[pos++] = fmt[i];
			continue;
		}
		i++;
		switch (fmt[i]) {
		case 'd': {
			int v = va_arg(args, int);
			if (v < 0) { str[pos++] = '-'; v = -v; }
			print_dec((unsigned long)v, str, &pos);
			break;
		}
		case 'u': {
			unsigned int v = va_arg(args, unsigned int);
			print_dec(v, str, &pos);
			break;
		}
		case 'x': case 'X': {
			unsigned int v = va_arg(args, unsigned int);
			print_hex(v, str, &pos);
			break;
		}
		case 's': {
			const char *s = va_arg(args, const char *);
			if (!s) s = "(null)";
			while (*s && pos < (int)size - 1) str[pos++] = *s++;
			break;
		}
		case 'c': {
			str[pos++] = (char)va_arg(args, int);
			break;
		}
		case 'l': {
			i++;
			switch (fmt[i]) {
			case 'd': {
				long v = va_arg(args, long);
				if (v < 0) { str[pos++] = '-'; v = -v; }
				print_dec((unsigned long)v, str, &pos);
				break;
			}
			case 'u': print_dec(va_arg(args, unsigned long), str, &pos); break;
			case 'x': case 'X': print_hex(va_arg(args, unsigned long), str, &pos); break;
			default: str[pos++] = '%'; str[pos++] = 'l'; str[pos++] = fmt[i]; break;
			}
			break;
		}
		default:
			str[pos++] = '%';
			str[pos++] = fmt[i];
			break;
		}
	}
	str[pos] = '\0';
	va_end(args);
	return pos;
}

int printf(const char *fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	int n = snprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	syscall(SYS_WRITE, (long)buf, (long)n, 1, 0);
	return n;
}
