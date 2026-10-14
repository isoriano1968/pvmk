/*
 * Freestanding basic types for pvmk (no host or libc headers).
 */
#ifndef PVMK_TYPES_H
#define PVMK_TYPES_H

typedef unsigned char	u8;
typedef unsigned short	u16;
typedef unsigned int	u32;
typedef signed char	s8;
typedef signed short	s16;
typedef signed int	s32;
typedef unsigned long	uintptr_t;
typedef unsigned long	size_t;

#define NULL	((void *)0)

typedef __builtin_va_list va_list;
#define va_start(ap, last)	__builtin_va_start(ap, last)
#define va_arg(ap, type)	__builtin_va_arg(ap, type)
#define va_end(ap)		__builtin_va_end(ap)

#define REG16(a)	(*(volatile u16 *)(uintptr_t)(a))
#define REG32(a)	(*(volatile u32 *)(uintptr_t)(a))
#define PEEK8(p, o)	(*(volatile u8 *)((u8 *)(p) + (o)))
#define PEEK16(p, o)	(*(volatile u16 *)((u8 *)(p) + (o)))
#define PEEK32(p, o)	(*(volatile u32 *)((u8 *)(p) + (o)))

#endif /* PVMK_TYPES_H */
