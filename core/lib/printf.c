#include "lib.h"

/*
 * PL011 UART — QEMU virt machine places it at 0x09000000.
 * This is a minimal driver for early boot console output.
 * Once the component framework is up, uart_pl011 component takes over.
 */

#define UART_BASE   0x09000000UL

/* PL011 register offsets */
#define UART_DR     0x000   /* Data Register */
#define UART_FR     0x018   /* Flag Register */
#define UART_FR_TXFF (1 << 5)  /* Transmit FIFO full */

static volatile uint8_t *const uart = (volatile uint8_t *)UART_BASE;

void uart_init(void)
{
    /* QEMU's PL011 works out of the box — no init needed for basic TX.
     * Real hardware would set baud rate, enable FIFOs, etc. */
}

void uart_putc(char c)
{
    /* Wait until TX FIFO is not full */
    while (*(volatile uint32_t *)(uart + UART_FR) & UART_FR_TXFF)
        ;
    *(volatile uint32_t *)(uart + UART_DR) = c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

/* Minimal kprintf — supports %s, %d, %u, %x, %p, %c, %% */

static void print_uint(uint64_t val, int base, int pad_width, char pad_char)
{
    char buf[20];
    int i = 0;

    if (val == 0) {
        buf[i++] = '0';
    } else {
        while (val > 0) {
            int digit = val % base;
            buf[i++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
            val /= base;
        }
    }

    /* Padding */
    for (int j = i; j < pad_width; j++)
        uart_putc(pad_char);

    /* Print in reverse */
    while (i > 0)
        uart_putc(buf[--i]);
}

static void print_int(int64_t val, int pad_width, char pad_char)
{
    if (val < 0) {
        uart_putc('-');
        val = -val;
        if (pad_width > 0)
            pad_width--;
    }
    print_uint((uint64_t)val, 10, pad_width, pad_char);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n')
                uart_putc('\r');
            uart_putc(*fmt++);
            continue;
        }

        fmt++; /* skip '%' */

        /* Parse optional '0' pad and width */
        char pad_char = ' ';
        int pad_width = 0;

        if (*fmt == '0') {
            pad_char = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            pad_width = pad_width * 10 + (*fmt - '0');
            fmt++;
        }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            uart_puts(s ? s : "(null)");
            break;
        }
        case 'd': {
            int64_t val = va_arg(ap, int);
            print_int(val, pad_width, pad_char);
            break;
        }
        case 'u': {
            uint64_t val = va_arg(ap, unsigned int);
            print_uint(val, 10, pad_width, pad_char);
            break;
        }
        case 'x': {
            uint64_t val = va_arg(ap, unsigned int);
            print_uint(val, 16, pad_width, pad_char);
            break;
        }
        case 'p': {
            uint64_t val = va_arg(ap, uint64_t);
            uart_puts("0x");
            print_uint(val, 16, 16, '0');
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            uart_putc(c);
            break;
        }
        case 'l': {
            fmt++; /* skip 'l' */
            if (*fmt == 'u') {
                uint64_t val = va_arg(ap, uint64_t);
                print_uint(val, 10, pad_width, pad_char);
            } else if (*fmt == 'x') {
                uint64_t val = va_arg(ap, uint64_t);
                print_uint(val, 16, pad_width, pad_char);
            } else if (*fmt == 'd') {
                int64_t val = va_arg(ap, int64_t);
                print_int(val, pad_width, pad_char);
            }
            break;
        }
        case '%':
            uart_putc('%');
            break;
        default:
            uart_putc('%');
            uart_putc(*fmt);
            break;
        }
        fmt++;
    }

    va_end(ap);
}
