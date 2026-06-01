#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <b1nix/syscall.h>

int putchar(int c)
{
	char ch = (char)c;
	syscall_dispatch(SYS_WRITE, 1, (u64)(usize)&ch, 1, 0, 0, 0);
	return c;
}

int puts(const char *s)
{
	syscall_dispatch(SYS_WRITE, 1, (u64)(usize)s, strlen(s), 0, 0, 0);
	putchar('\n');
	return 0;
}

static void print_dec_to(char *tmp, int *len, u64 value)
{
	if (value >= 10) {
		print_dec_to(tmp, len, value / 10);
	}
	tmp[(*len)++] = '0' + (value % 10);
}

static void print_hex_to(char *tmp, int *len, u64 value)
{
	const char *hex = "0123456789abcdef";
	if (value >= 16) {
		print_hex_to(tmp, len, value / 16);
	}
	tmp[(*len)++] = hex[value % 16];
}

static void append_char(char *str, size_t size, int *pos, char ch)
{
	if (*pos < (int)size - 1) {
		str[(*pos)++] = ch;
	}
}

static void append_string(char *str, size_t size, int *pos, const char *s)
{
	if (!s) s = "(null)";
	while (*s) {
		append_char(str, size, pos, *s++);
	}
}

static void append_number(char *str, size_t size, int *pos, const char *digits,
                          int len, int negative, int width, int zero_pad)
{
	int total = len + (negative ? 1 : 0);
	char pad = zero_pad ? '0' : ' ';
	if (!zero_pad) {
		while (width > total) {
			append_char(str, size, pos, pad);
			width--;
		}
	}
	if (negative) {
		append_char(str, size, pos, '-');
	}
	if (zero_pad) {
		while (width > total) {
			append_char(str, size, pos, pad);
			width--;
		}
	}
	for (int i = 0; i < len; i++) {
		append_char(str, size, pos, digits[i]);
	}
}

static int vsnprintf_impl(char *str, size_t size, const char *fmt, va_list args)
{
	int pos = 0;

	for (int i = 0; fmt[i]; i++) {
		if (fmt[i] != '%') {
			append_char(str, size, &pos, fmt[i]);
			continue;
		}

		i++;
		int zero_pad = 0;
		int width = 0;
		if (fmt[i] == '0') {
			zero_pad = 1;
			i++;
		}
		while (fmt[i] >= '0' && fmt[i] <= '9') {
			width = width * 10 + (fmt[i] - '0');
			i++;
		}

		switch (fmt[i]) {
		case 'd': {
			int val = va_arg(args, int);
			int negative = val < 0;
			u64 out = negative ? (u64)(-val) : (u64)val;
			char tmp[32];
			int len = 0;
			print_dec_to(tmp, &len, out);
			append_number(str, size, &pos, tmp, len, negative, width, zero_pad);
			break;
		}
		case 'u': {
			char tmp[32];
			int len = 0;
			print_dec_to(tmp, &len, va_arg(args, unsigned int));
			append_number(str, size, &pos, tmp, len, 0, width, zero_pad);
			break;
		}
		case 'x':
		case 'X': {
			char tmp[32];
			int len = 0;
			print_hex_to(tmp, &len, va_arg(args, unsigned int));
			append_number(str, size, &pos, tmp, len, 0, width, zero_pad);
			break;
		}
		case 's':
			append_string(str, size, &pos, va_arg(args, const char *));
			break;
		case 'c':
			append_char(str, size, &pos, (char)va_arg(args, int));
			break;
		case 'p': {
			void *p = va_arg(args, void *);
			append_string(str, size, &pos, "0x");
			char tmp[32];
			int len = 0;
			print_hex_to(tmp, &len, (u64)(usize)p);
			append_number(str, size, &pos, tmp, len, 0, width, zero_pad);
			break;
		}
		case 'l': {
			if (!fmt[i + 1]) break;
			i++;
			switch (fmt[i]) {
			case 'd': {
				long val = va_arg(args, long);
				int negative = val < 0;
				u64 out = negative ? (u64)(-val) : (u64)val;
				char tmp[32];
				int len = 0;
				print_dec_to(tmp, &len, out);
				append_number(str, size, &pos, tmp, len, negative, width, zero_pad);
				break;
			}
			case 'u': {
				char tmp[32];
				int len = 0;
				print_dec_to(tmp, &len, va_arg(args, unsigned long));
				append_number(str, size, &pos, tmp, len, 0, width, zero_pad);
				break;
			}
			case 'x':
			case 'X': {
				char tmp[32];
				int len = 0;
				print_hex_to(tmp, &len, va_arg(args, unsigned long));
				append_number(str, size, &pos, tmp, len, 0, width, zero_pad);
				break;
			}
			default:
				append_char(str, size, &pos, '%');
				append_char(str, size, &pos, 'l');
				append_char(str, size, &pos, fmt[i]);
				break;
			}
			break;
		}
		case '%':
			append_char(str, size, &pos, '%');
			break;
		default:
			append_char(str, size, &pos, '%');
			append_char(str, size, &pos, fmt[i]);
			break;
		}
	}

	if (size > 0) str[pos < (int)size ? pos : (int)size - 1] = '\0';
	return pos;
}

int snprintf(char *str, size_t size, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int len = vsnprintf_impl(str, size, fmt, args);
	va_end(args);
	return len;
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list args)
{
	return vsnprintf_impl(str, size, fmt, args);
}

int printf(const char *fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	int len = vsnprintf_impl(buf, sizeof(buf), fmt, args);
	va_end(args);
	syscall_dispatch(SYS_WRITE, 1, (u64)(usize)buf, len, 0, 0, 0);
	return len;
}
