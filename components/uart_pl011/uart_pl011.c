/*
 * uart_pl011 — PL011 UART character-device component.
 *
 * Slice 8.0b: replaces the original smoke-test handle_msg (which only
 * tracked message counts and checked UART_MSG_WRITE) with the IDL-
 * generated nx_char_device_dispatch shim.  The component now implements
 * struct nx_char_device_ops and delegates handle_msg entirely to the
 * generated dispatcher.
 *
 * Implements:
 *   write(buf, len)  — writes len bytes to the PL011 TX FIFO via uart_putc.
 *   rx_byte(byte)    — receives one byte from IRQ context (no-op in v1;
 *                      a future RX ring-buffer will land here).
 */

#include "framework/char_device_dispatch.h"
#include "framework/component.h"
#include "framework/ipc.h"
#include "interfaces/char_device.h"

#if __STDC_HOSTED__
#include <string.h>
#else
#include "core/lib/lib.h"
#endif

struct uart_pl011_state {
    unsigned init_called;
    unsigned enable_called;
};

/* ---------- char_device ops ------------------------------------------ */

static int64_t uart_pl011_write(void *self, const void *buf, size_t len)
{
    (void)self;
#if !__STDC_HOSTED__
    const char *p = buf;
    for (size_t i = 0; i < len; i++)
        uart_putc(p[i]);
#else
    (void)buf;
#endif
    return (int64_t)len;
}

static void uart_pl011_rx_byte(void *self, uint8_t byte)
{
    /* v1: no RX ring buffer yet; bytes delivered from IRQ context are
     * silently dropped.  A future slice wires this to a per-device
     * receive queue and wakes any blocked reader. */
    (void)self;
    (void)byte;
}

static const struct nx_char_device_ops uart_pl011_char_device_ops = {
    .write   = uart_pl011_write,
    .rx_byte = uart_pl011_rx_byte,
};

/* ---------- component lifecycle -------------------------------------- */

static int uart_pl011_init(void *self)
{
    struct uart_pl011_state *s = self;
    s->init_called++;
    return 0;
}

static int uart_pl011_enable(void *self)
{
    struct uart_pl011_state *s = self;
    s->enable_called++;
    return 0;
}

static int uart_pl011_handle_msg(void *self, struct nx_ipc_message *msg)
{
    return nx_char_device_dispatch(self, &uart_pl011_char_device_ops, msg);
}

static const struct nx_component_ops uart_pl011_ops = {
    .init       = uart_pl011_init,
    .enable     = uart_pl011_enable,
    .handle_msg = uart_pl011_handle_msg,
};

NX_COMPONENT_REGISTER_NO_DEPS_IFACE(uart_pl011,
                                    struct uart_pl011_state,
                                    &uart_pl011_ops,
                                    &uart_pl011_char_device_ops);
