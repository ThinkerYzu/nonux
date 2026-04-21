#ifndef NONUX_LIB_H
#define NONUX_LIB_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* string.c */
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);

/* printf.c */
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void kprintf(const char *fmt, ...);

#endif /* NONUX_LIB_H */
