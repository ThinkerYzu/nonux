/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/char_device.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_INTERFACE_CHAR_DEVICE_H
#define NONUX_INTERFACE_CHAR_DEVICE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Character-device interface — byte-stream peripherals (UART, virtio-console, …).
 *
 * Slice 8.0pre.3 introduces this interface; first concrete consumer is
 * `components/uart_pl011/` once slice 8.0b activates the generated
 * handle_msg shims.  The interface is intentionally minimal: a
 * `write` op for buffered output (the path syscall layer already
 * uses for serial console) and an IRQ-entry `rx_byte` op for input
 * bytes delivered from an ISR.  More ops (flush, get_modem_status,
 * set_baud) will land when a real driver demands them.
 *
 * Ownership.
 *   `write`'s `buf` is borrowed for the duration of the call — drivers
 *   copy any bytes they intend to keep before returning.  `rx_byte`
 *   takes a single byte by value; nothing to own.
 *
 * Concurrency.
 *   `write` runs in task context (caller blocks for the duration);
 *   `rx_byte` runs in IRQ context (ISR calls the framework's
 *   `nx_<iface>_rx_byte_from_irq` helper which bounds-checks pool
 *   allocation and enqueues the message for the dispatcher kthread
 *   to handle later).  Drivers must not block in `rx_byte`.
 *
 * Error convention.
 *   `write` returns POSIX-style byte-count-or-status (>=0 on success,
 *   negative `NX_E*` on failure) matching `nx_fs_ops.write`.
 *   `rx_byte` is `void` — the IRQ path cannot recover from a failed
 *   delivery (pool exhaustion drops the byte; the framework counts
 *   drops via a future telemetry hook).
 */

struct nx_char_device_ops {
    /*
     * Write `len` bytes from `buf` to the device.  Blocking in v1
     * (uart_pl011 polls the TX-empty bit per byte).  Future drivers
     * MAY return a partial count if the device's TX FIFO fills before
     * `len` bytes are accepted; callers retry with the remaining tail.
     *
     * Returns:
     *   >= 0       — bytes written.
     *   NX_EINVAL  — NULL buf with non-zero len.
     *   NX_EIO     — device-level failure (not used by uart_pl011 today).
     */
    int64_t (*write)(void *self, const void *buf, size_t len);

    /*
     * Deliver one received byte to the driver.  Called from ISR
     * context via the generated `nx_char_device_rx_byte_from_irq`
     * helper, which grabs a slot from a fixed-size pre-built message
     * pool and enqueues the message for later dispatch on the
     * framework dispatcher kthread.
     *
     * The driver's task-context handler runs after the ISR returns;
     * it typically pushes the byte onto a per-device receive ring and
     * wakes any reader blocked in `read` (when that op lands).
     *
     * No return value: the IRQ path cannot back-propagate failures.
     * Pool-exhaustion drops the byte silently in v1; a future
     * telemetry hook will count drops.
     */
    void (*rx_byte)(void *self, uint8_t byte);
};

#endif /* NONUX_INTERFACE_CHAR_DEVICE_H */
