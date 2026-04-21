# uart_pl011

PL011 UART component — the first real nonux component, introduced in
slice 3.9a as the minimum proof that the kernel's boot walker actually
instantiates and runs components from composition.

## Interface

- **iface:** `char_device`
- **Bound by default:** `kernel.json["components"]["char_device.serial"]`.
- **Dependencies:** none.

## Behaviour

- `init` — increments `state.init_called`. The underlying PL011
  hardware is already brought up by `core/boot/boot.c:uart_init()`
  before the framework bootstraps, so the component has no hardware
  work to do.
- `enable` — increments `state.enable_called`.
- `handle_msg` — increments `state.messages_handled`. For a
  `msg_type = UART_MSG_WRITE` message it writes the raw payload
  bytes through `uart_putc`.

State is tiny (three `unsigned` counters) so a kernel test can peek
at the component's `impl` pointer after bootstrap and verify init /
enable actually ran.

## Why this component exists

Slice 3.9a's goal is to prove the boot walker end-to-end:

```
linker section  →  nx_framework_bootstrap()
    →  nx_slot_register(char_device.serial)
    →  nx_component_register(uart_pl011)
    →  nx_slot_swap(char_device.serial ← uart_pl011)
    →  nx_component_init(uart_pl011)
    →  nx_component_enable(uart_pl011)
    →  nx_graph_snapshot_to_json() [to serial]
```

A stub-only component wouldn't exercise `handle_msg`, `init`, or
`enable`. The three-counter state is the cheapest way to observe that
each hook actually fired.

## Future

Slice 3.9b will replace this component's `handle_msg` with a real
lock-free producer feeding a dedicated dispatcher thread. Phase 5
promotes `char_device` into a proper typed interface, at which point
the slot's `iface` tag becomes load-bearing and `uart_pl011` grows
read / ioctl callbacks.
