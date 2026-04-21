/*
 * uart_pl011 — the thinnest possible component that proves slice 3.9a
 * boot-walking actually runs something.
 *
 * The kernel's `boot_main` initialises the PL011 UART before running
 * `nx_framework_bootstrap()`, so by the time init/enable fire the low-
 * level hardware is already up.  This component is therefore a pure
 * handler: incoming messages carry a length-prefixed string in
 * msg->payload and the handler writes it byte-by-byte through the
 * existing `uart_putc` helper.
 *
 * No dependencies, no state.  Manifest declares iface "char_device".
 * kernel.json's "char_device.serial" slot picks this up by name.
 */

#include "framework/component.h"
#include "framework/ipc.h"

#if __STDC_HOSTED__
#include <string.h>
#else
#include "core/lib/lib.h"
#endif

/* Keep a tiny "hello from a component" marker we can poke to confirm
 * init/enable actually ran on the device.  Observable via handle_msg
 * and via the kernel test. */
struct uart_pl011_state {
    unsigned init_called;
    unsigned enable_called;
    unsigned messages_handled;
};

/* Generic "write payload as bytes" message type. */
#define UART_MSG_WRITE 1

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
    struct uart_pl011_state *s = self;
    s->messages_handled++;
    if (msg->msg_type == UART_MSG_WRITE && msg->payload && msg->payload_len) {
#if __STDC_HOSTED__
        /* Host tests never invoke handle_msg against this descriptor
         * today; if that changes, wire to stdout.  For now this branch
         * is a structural no-op — keeps the body tiny and obviously
         * correct. */
        (void)msg;
#else
        const char *p = msg->payload;
        for (uint32_t i = 0; i < msg->payload_len; i++)
            uart_putc(p[i]);
#endif
    }
    return 0;
}

static const struct nx_component_ops uart_pl011_ops = {
    .init       = uart_pl011_init,
    .enable     = uart_pl011_enable,
    .handle_msg = uart_pl011_handle_msg,
};

NX_COMPONENT_REGISTER_NO_DEPS(uart_pl011,
                              struct uart_pl011_state,
                              &uart_pl011_ops);
