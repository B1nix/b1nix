#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <b1nix/syscall.h>

int putchar(int c)
{
	char ch = (char)c;
	syscall_dispatch(SYS_WRITE, (u64)(usize)&ch, 1, 1, 1);
	return c;
}

int puts(const char *s)
{
	syscall_dispatch(SYS_WRITE, (u64)(usize)s, strlen(s), 1, 1);
	putchar('\n');
	return 0;
}

static void print_dec(u64 value, char *buf, int *pos)
{
	if (value >= 10) {
		print_dec(value / 10, buf, pos);
	}
	buf[(*pos)++] = '0' + (value % 10);
}

static void print_hex(u64 value, char *buf, int *pos)
{
	const char *hex = "0123456789abcdef";
	if (value >= 16) {
		print_hex(value / 16, buf, pos);
	}
	buf[(*pos)++] = hex[value % 16];
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
			int val = va_arg(args, int);
			if (val < 0) {
				str[pos++] = '-';
				val = -val;
			}
			print_dec((u64)val, str, &pos);
			break;
		}
		case 'u': {
			unsigned int val = va_arg(args, unsigned int);
			print_dec(val, str, &pos);
			break;
		}
		case 'x':
		case 'X': {
			unsigned int val = va_arg(args, unsigned int);
			print_hex(val, str, &pos);
			break;
		}
		case 's': {
			const char *s = va_arg(args, const char *);
			if (!s) s = "(null)";
			while (*s && pos < (int)size - 1) {
				str[pos++] = *s++;
			}
			break;
		}
		case 'c': {
			char c = (char)va_arg(args, int);
			str[pos++] = c;
			break;
		}
		case 'p': {
			void *p = va_arg(args, void *);
			str[pos++] = '0';
			str[pos++] = 'x';
			print_hex((u64)(usize)p, str, &pos);
			break;
		}
		case 'l': {
			/* Handle %ld, %lx, %lu */
			if (fmt[i+1]) {
				i++;
				long val;
				switch (fmt[i]) {
				case 'd':
					val = va_arg(args, long);
					if (val < 0) {
						str[pos++] = '-';
						val = -val;
					}
					print_dec((u64)val, str, &pos);
					break;
				case 'u':
					print_dec(va_arg(args, unsigned long), str, &pos);
					break;
				case 'x':
				case 'X':
					print_hex(va_arg(args, unsigned long), str, &pos);
					break;
				default:
					str[pos++] = '%';
					str[pos++] = 'l';
					str[pos++] = fmt[i];
					break;
				}
			}
			break;
		}
		case '%':
			str[pos++] = '%';
			break;
		default:
			str[pos++] = '%';
			str[pos++] = fmt[i];
			break;
		}
	}
	
	va_end(args);
	str[pos] = '\0';
	return pos;
}

int printf(const char *fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	
	int len = snprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	
	syscall_dispatch(SYS_WRITE, (u64)(usize)buf, len, 1, 1);
	return len;
}
