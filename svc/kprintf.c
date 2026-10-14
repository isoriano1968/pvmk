/*
 * Minimal formatted output and string helpers for freestanding pvmk code.
 * No 64-bit arithmetic (68060 would trap on 64-bit MUL/DIV).
 */
#include "pvmk/kprintf.h"
#include "pvmk/log.h"

static void
out_str(void (*out)(int), const char *s, int width, int left)
{
	int len = (int)strlen(s);

	while (!left && len < width--)
		out(' ');
	while (*s)
		out(*s++);
	while (left && len < width--)
		out(' ');
}

static void
out_num(void (*out)(int), u32 v, unsigned base, int upper, int neg,
    int width, int pad)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char buf[12];
	int i = 0;

	do {
		buf[i++] = digits[v % base];
		v /= base;
	} while (v);
	if (neg) {
		if (pad == '0') {
			out('-');
			width--;
		} else
			buf[i++] = '-';
	}
	while (i < width--)
		out(pad);
	while (i)
		out(buf[--i]);
}

void
kvprintf(void (*out)(int), const char *fmt, va_list ap)
{
	for (; *fmt; fmt++) {
		int pad = ' ', width = 0, left = 0;
		s32 sv;

		if (*fmt != '%') {
			out(*fmt);
			continue;
		}
		fmt++;
		if (*fmt == '-') {
			left = 1;
			fmt++;
		}
		if (*fmt == '0') {
			pad = '0';
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		if (*fmt == 'l')
			fmt++;
		switch (*fmt) {
		case 'c':
			out(va_arg(ap, int));
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			out_str(out, s ? s : "(null)", width, left);
			break;
		}
		case 'd':
			sv = va_arg(ap, s32);
			out_num(out, sv < 0 ? (u32)-sv : (u32)sv, 10, 0, sv < 0,
			    width, pad);
			break;
		case 'u':
			out_num(out, va_arg(ap, u32), 10, 0, 0, width, pad);
			break;
		case 'x':
		case 'X':
			out_num(out, va_arg(ap, u32), 16, *fmt == 'X', 0, width,
			    pad);
			break;
		case 'p':
			out_num(out, (u32)(uintptr_t)va_arg(ap, void *), 16, 0, 0,
			    8, '0');
			break;
		case '%':
			out('%');
			break;
		default:
			out('%');
			out(*fmt);
			break;
		}
	}
}

void
kprintf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	kvprintf(con_putc, fmt, ap);
	va_end(ap);
}

void *
memset(void *s, int c, size_t n)
{
	u8 *p = s;

	while (n--)
		*p++ = (u8)c;
	return s;
}

void *
memcpy(void *d, const void *s, size_t n)
{
	u8 *dp = d;
	const u8 *sp = s;

	while (n--)
		*dp++ = *sp++;
	return d;
}

size_t
strlen(const char *s)
{
	const char *p = s;

	while (*p)
		p++;
	return (size_t)(p - s);
}
