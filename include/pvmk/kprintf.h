#ifndef PVMK_KPRINTF_H
#define PVMK_KPRINTF_H

#include "pvmk/types.h"

/*
 * Formatted output to the debug console. Supports %c %s %d %u %x %X %p
 * with optional '-' or '0' flag and field width, and the 'l' length modifier.
 */
void kprintf(const char *fmt, ...);
void kvprintf(void (*out)(int c), const char *fmt, va_list ap);

void *memset(void *s, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
size_t strlen(const char *s);

#endif /* PVMK_KPRINTF_H */
