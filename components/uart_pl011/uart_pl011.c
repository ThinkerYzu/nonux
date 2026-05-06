/*
 * uart_pl011 — PL011 UART character-device component.
 *
 * Slice 9b.1: adds read(id, buf, cap) op — routes to nx_console_read.
 * `id` is ignored (singleton device); a future multi-device scenario
 * would use it to select the instance.
 *
 * Implements:
 *   write(buf, len)       — writes len bytes to the PL011 TX FIFO.
 *   read(id, buf, cap)    — blocks until bytes available; reads from RX.
 *   rx_byte(byte)         — IRQ-context byte delivery (no-op in v1).
 */

#include "framework/char_device_dispatch.h"
#include "framework/component.h"
#include "framework/ipc.h"
#include "interfaces/char_device.h"

#if !__STDC_HOSTED__
#include "framework/console.h"
#endif

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
    return (int64_t)nx_console_write(buf, len);
#else
    (void)buf;
    return (int64_t)len;
#endif
}

static int64_t uart_pl011_read(void *self, uint32_t id, void *buf, size_t cap)
{
    (void)self;
    (void)id;    /* singleton — id ignored */
    if (cap == 0) return 0;
    if (!buf) return NX_EINVAL;
#if !__STDC_HOSTED__
    return (int64_t)nx_console_read(buf, cap);
#else
    (void)buf;
    return 0;    /* host build: EOF */
#endif
}

static void uart_pl011_rx_byte(void *self, uint8_t byte)
{
    (void)self;
    (void)byte;
}

static const struct nx_char_device_ops uart_pl011_char_device_ops = {
    .write   = uart_pl011_write,
    .read    = uart_pl011_read,
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
