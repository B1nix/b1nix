#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <b1nix/klog.h>
#include <b1nix/syscall.h>

/*
 * A format string bundled with its arguments, for %pV. Declared here as well as
 * in <linux/printk.h> — the two must agree on the layout, and this file is on
 * b1nix's side of the shim boundary and cannot include that header. It is two
 * pointers and has been since Linux introduced it.
 */
struct va_format {
	const char *fmt;
	va_list *va;
};

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

/* %x and %X differ only in case, and the difference is load-bearing: the
 * /proc files this kernel writes with %X — /proc/net/route, /proc/net/tcp,
 * /proc/net/ipv6_route — are uppercase on Linux, and anything that string
 * matches them (rather than parsing with a case-insensitive %x) reads a
 * lowercase kernel as having no such route at all. */
static void print_hex_case_to(char *tmp, int *len, u64 value, int upper)
{
	const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	if (value >= 16) {
		print_hex_case_to(tmp, len, value / 16, upper);
	}
	tmp[(*len)++] = hex[value % 16];
}

static void print_hex_to(char *tmp, int *len, u64 value)
{
	print_hex_case_to(tmp, len, value, 0);
}

static void append_char(char *str, size_t size, int *pos, char ch)
{
	if (*pos < (int)size - 1) {
		str[(*pos)++] = ch;
	}
}

static void append_string(char *str, size_t size, int *pos, const char *s)
{
	if (!s || (uintptr_t)s < 4096) s = "(null)";
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
		/* %i is %d. Missing it was not cosmetic: imported code uses it for
		 * every "expected %i, found %i" mismatch report, so the values that
		 * say what is wrong were the part that did not print. */
		case 'i':
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
			print_hex_case_to(tmp, &len, va_arg(args, unsigned int),
			                  fmt[i] == 'X');
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
			/*
			 * Linux's pointer extensions. Imported drivers use these heavily —
			 * every drm_err() and drm_info() message is a %pV, and a lockdep or
			 * WARN backtrace is %pS — so without them the diagnostics that say
			 * what a driver is doing come out as a raw pointer followed by a
			 * stray letter, which is how an i915 probe failure looked like
			 * nothing at all.
			 */
			switch (fmt[i + 1]) {
			case '4': {
				/* %p4cc — a fourcc pixel format, printed as the four
				 * characters it is spelled with. DRM reports every plane's
				 * format this way. */
				if (fmt[i + 2] == 'c' && fmt[i + 3] == 'c') {
					i += 3;
					const u32 *fourcc = va_arg(args, const u32 *);
					if (fourcc) {
						u32 v = *fourcc;
						for (int c = 0; c < 4; c++) {
							char ch = (char)((v >> (c * 8)) & 0xff);
							append_char(str, size, &pos,
							            (ch >= 0x20 && ch < 0x7f) ? ch : '.');
						}
					}
					break;
				}
				/* Not a fourcc: fall through to the plain pointer. */
				void *p4 = va_arg(args, void *);
				append_string(str, size, &pos, "0x");
				char t4[32];
				int l4 = 0;
				print_hex_to(t4, &l4, (u64)(usize)p4);
				append_number(str, size, &pos, t4, l4, 0, width, zero_pad);
				break;
			}
			case 'V': {
				/* A nested format string and its arguments, as one argument.
				 * The va_list is passed by pointer because the caller keeps
				 * using it after we return. */
				i++;
				const struct va_format *vaf =
					va_arg(args, const struct va_format *);
				if (vaf && vaf->fmt && vaf->va) {
					/*
					 * Formatted straight into the remaining output rather than
					 * through a scratch buffer: a nested buffer big enough to
					 * be useful is hundreds of bytes of kernel stack, on a
					 * path every driver message takes, and some of those run
					 * on the short stacks.
					 */
					va_list copy;
					va_copy(copy, *vaf->va);
					if (pos < (int)size - 1) {
						int n = vsnprintf_impl(str + pos, size - (size_t)pos,
						                       vaf->fmt, copy);
						pos += n;
						if (pos > (int)size - 1)
							pos = (int)size - 1;
					}
					va_end(copy);
				}
				break;
			}
			case 's':
			case 'S':
			case 'f':
			case 'F': {
				/* A code address as its symbol name, plus the offset into it.
				 * kallsyms is what makes this readable; without a match it
				 * falls back to the bare address rather than printing a name
				 * that is not the right one. */
				i++;
				void *p = va_arg(args, void *);
				u64 off = 0;
				const char *sym = ksym_lookup((u64)(usize)p, &off);
				if (sym) {
					append_string(str, size, &pos, sym);
					if (off) {
						char tmp[32];
						int len = 0;
						append_string(str, size, &pos, "+0x");
						print_hex_to(tmp, &len, off);
						append_number(str, size, &pos, tmp, len, 0, 0, 0);
					}
				} else {
					char tmp[32];
					int len = 0;
					append_string(str, size, &pos, "0x");
					print_hex_to(tmp, &len, (u64)(usize)p);
					append_number(str, size, &pos, tmp, len, 0, 0, 0);
				}
				break;
			}
			default: {
				void *p = va_arg(args, void *);
				append_string(str, size, &pos, "0x");
				char tmp[32];
				int len = 0;
				print_hex_to(tmp, &len, (u64)(usize)p);
				append_number(str, size, &pos, tmp, len, 0, width, zero_pad);
				break;
			}
			}
			break;
		}
		case 'l': {
			if (!fmt[i + 1]) break;
			i++;
			/*
			 * %ll is the same width as %l here — both are 64-bit — so the
			 * second 'l' is consumed and the conversion handled once. Without
			 * this the whole specifier fell through to the default and printed
			 * itself, which is how a GGTT range came out as "[%llx, %llx]".
			 */
			if (fmt[i] == 'l' && fmt[i + 1])
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
				print_hex_case_to(tmp, &len, va_arg(args, unsigned long),
				                  fmt[i] == 'X');
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
