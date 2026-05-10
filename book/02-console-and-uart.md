# The console: kprintf and the UART

When `boot_main()` ran for the first time at the end of chapter 1,
the very first thing your terminal showed was a banner:

```
========================================
  nonux — composable microkernel
  ARM64 / QEMU virt
========================================
```

Where did those bytes come from? They came from a few lines of C
code that look as ordinary as anything you might write in a hosted
program:

```c
kprintf("\n");
kprintf("========================================\n");
kprintf("  nonux — composable microkernel\n");
```

This chapter answers: **what happens between that line of C and a
character on your screen?**

The answer goes through some ideas that come up over and over in
kernel work: how software talks to a hardware device, the
difference between sending a byte and waiting for one, and what
an *interrupt* actually does to the CPU. By the end you should be
able to read the output side and the input side of any byte-stream
device — UARTs, keyboards, network controllers, anything where
the chip and the kernel push bytes back and forth.

The files we'll be talking about:

- [`core/lib/lib.h`](../core/lib/lib.h) — declares `uart_init`,
  `uart_putc`, `uart_puts`, and `kprintf`.
- [`core/lib/printf.c`](../core/lib/printf.c) — the early-boot UART
  driver and the format-string code for `kprintf`.
- [`framework/console.h`](../framework/console.h),
  [`framework/console.c`](../framework/console.c) — the input side:
  the RX interrupt handler, the byte ring, and the blocking
  `nx_console_read`.
- [`components/uart_pl011/uart_pl011.c`](../components/uart_pl011/uart_pl011.c) —
  the same UART, packaged as a *component* so userspace programs
  can write to it through normal **file descriptors**. We'll touch
  on this lightly here; the full framework story comes later.

---

## Terms you'll see

- **Console.** The text channel the kernel uses to talk to the
  outside world — output for messages, input for keys typed by the
  user. On a desktop computer, the console is the screen and
  keyboard; in nonux on QEMU, it's the terminal you launched
  `make run` from.
- **UART.** Short for **Universal Asynchronous Receiver-Transmitter**.
  An old, simple kind of serial port: a chip that sends and
  receives one byte at a time over a pair of wires. "Asynchronous"
  here means the two ends agree on a *speed* in advance (the baud
  rate) rather than sharing a clock signal.
- **PL011.** A specific UART chip designed by ARM. The "PL" stands
  for "PrimeCell", ARM's family name for reusable peripheral
  designs. QEMU's `virt` machine includes a PL011 at a fixed
  address. nonux's driver is written for that exact chip.
- **MMIO (Memory-Mapped I/O).** A way of talking to a hardware
  device by reading from and writing to special memory addresses.
  Loads and stores at those addresses don't touch RAM at all —
  they go to the device. We'll see this in detail soon.
- **Device register.** A named storage location *inside* a hardware
  device, addressable through MMIO. Each register has a fixed
  purpose: one might be "the next byte to send", another might be
  "current status flags". A device's *programming guide* lists its
  registers.
- **Device driver.** Code in the kernel that knows how to talk to
  one particular device. The PL011 driver is a few hundred lines of
  C; a real ethernet driver might be thousands.
- **`volatile`.** A C qualifier you put on a pointer or a variable.
  It tells the compiler "every load and every store really happens
  — don't merge them, don't reorder them, don't skip them". MMIO
  needs this because the hardware *cares* about every access; the
  compiler's normal optimizations would break things.
- **FIFO (First-In, First-Out).** A queue. The first thing put in
  is the first thing taken out. UART chips have small
  FIFOs in hardware to smooth out short bursts of bytes.
- **Ring buffer.** A FIFO implemented with a fixed-size array and
  two indices: a *head* (where the next byte gets written) and a
  *tail* (where the next byte gets read). When either index walks
  off the end of the array, it wraps back to the start — that's
  the "ring".
- **IRQ (Interrupt Request).** A signal from a device asking the
  CPU to stop what it's doing and run a short handler. The CPU
  pauses, runs the handler, then resumes whatever it was doing.
  We'll see one fire in this chapter; the deeper hardware
  mechanics get a chapter of their own later.
- **ISR (Interrupt Service Routine).** The handler function that
  runs when an IRQ fires. ISRs run at EL1 with interrupts masked
  and have to be short — they're not allowed to block, sleep, or
  call code that could.
- **Format string.** The first argument to `printf`-style functions
  — a string with `%d`, `%s`, etc. placeholders that get filled in
  from the remaining arguments.
- **POSIX.** A standard set of names and rules that Unix-like
  operating systems follow — for files, processes, system calls,
  signals, and so on. When you see `read`, `write`, `fd`, or
  `SIGTERM` in this book, those names come from POSIX.
- **System call (syscall).** A request from a user program for
  the kernel to do something the program isn't allowed to do
  directly — open a file, write to a device, allocate memory.
  The user code triggers a special CPU instruction that traps
  into the kernel; the kernel runs the request and returns. The
  full mechanics get a dedicated chapter later. In this chapter,
  "the syscall layer" just means the kernel side of that boundary.
- **File descriptor (fd).** A small integer a user program uses
  to refer to an open file or device. POSIX reserves three by
  convention: **fd 0** is standard input (`stdin`), **fd 1** is
  standard output (`stdout`), **fd 2** is standard error
  (`stderr`). `read(0, ...)` reads from stdin; `write(1, ...)`
  writes to stdout.
- **Kernel thread (kthread).** A thread of execution that lives
  entirely inside the kernel — it doesn't belong to any user
  process. Used for background work the kernel itself wants to
  do (drivers, dispatchers, the idle loop). Kthreads run at EL1
  from start to finish.
- **Signal.** A short, named event the kernel can deliver to a
  process — for example, "you've been asked to terminate"
  (`SIGTERM`) or "a segmentation fault happened" (`SIGSEGV`). In
  v1 nonux, a delivered `SIGTERM` simply terminates the process
  at its next opportunity; full handler-driven signals are
  deferred to a later chapter.

---

## What we're building toward

A **console** is the kernel's text channel — output for log
messages, input for typed characters. Every kernel needs one. It's
how you find out what's going on (especially when something has
gone wrong), and on systems with a shell, it's how the user talks
to programs.

In nonux, the console has **two paths into the same physical UART**:

1. **A direct path** used by the kernel itself. `kprintf` formats
   a message and pokes bytes straight at the UART's hardware
   registers. No queues, no async, no userspace involvement. This
   path works from the moment `boot_main` starts running.

2. **A component path** used by userspace. When a user program
   writes to fd 1 (the standard-output stream `stdout`), the call
   goes through the kernel's **syscall** layer, then through the
   component framework, into a component called `uart_pl011`, and
   finally *also* ends up at the same UART registers.

Most of this chapter walks the **direct path**, because that's the
short, easy story and it covers all the hardware ideas you need.
Then we'll briefly meet the component side at the end. The
component framework gets its own deep dive in a later chapter; here
we'll just establish that it exists and what role it plays for the
console.

The **input side** — typing a key into your terminal and having
the kernel see it — has only one path: an **IRQ**-driven ring
buffer in `framework/console.c`. (An IRQ — interrupt request —
is the device's way of poking the CPU when something arrives;
we'll see one fire later in this chapter.) We'll cover that too.

---

## What's a UART?

A **UART** is one of the oldest and simplest ways for two devices
to exchange data over a wire. The acronym, "Universal Asynchronous
Receiver-Transmitter", describes its job:

- **Transmitter:** the side that sends bytes out.
- **Receiver:** the side that takes bytes in.
- **Asynchronous:** the two ends don't share a clock signal.
  Instead they agree in advance on a *bit rate* (commonly called
  the **baud rate**) — like 115200 bits per second. Each side
  uses its own clock to time when the bits go out or come in.

A real UART connection has just three signal wires:

```
   Device A                        Device B
  ┌────────┐                      ┌────────┐
  │   TX   │ ──────────────────▶  │   RX   │
  │   RX   │ ◀──────────────────  │   TX   │
  │   GND  │ ◀──────────────────▶ │   GND  │
  └────────┘                      └────────┘
```

A is sending a byte? It clocks the bits out one at a time on its TX
wire. B's RX wire is connected to A's TX wire, so B sees those bits
and reassembles them. The reverse goes the other way. Ground is
just the shared zero level both sides need to interpret voltages.

UARTs ship one byte at a time, framed with a *start bit* before and
a *stop bit* after each byte. They are slow by modern standards —
115200 baud is around 11 KiB/sec — but **dead simple** to wire up.
They were the standard way that minicomputers, embedded boards,
modems, and serial terminals talked to each other for decades.

> **Side note: where you still find UARTs.** UARTs aren't gone.
> They're hidden inside USB-to-serial cables, GPS receivers,
> Bluetooth modules, board-management controllers, and most
> developer boards (Raspberry Pi, ESP32, every ARM dev kit). They
> remain the go-to debug interface because they need almost
> nothing — no driver stack, no negotiation, just power and
> bit-banged voltage. You can be debugging a brand-new chip on
> day one over its UART before any of its other peripherals work.

### Why the kernel uses one

A kernel running on bare metal has a problem: it has nothing to
print to. There's no graphics card driver yet (graphics is huge
and complicated). There's no terminal emulator, no framebuffer
font. Even if there were a screen, the kernel couldn't easily put
text on it without a lot of code.

A UART, by contrast, takes one line: write a byte, the byte goes
out the TX wire. Whatever's on the other end (a real terminal, a
serial-port logger, QEMU's window) shows the character.

So **every serious bare-metal kernel starts with a UART driver as
its first console**. Real ones print boot messages on a UART that
goes to a header pin you connect a USB-serial dongle to. nonux
prints to a UART that goes to QEMU's terminal window. Same idea,
different other end.

### The PL011 specifically

**PL011** is a particular UART chip, designed by ARM as part of
their PrimeCell family. It's the UART you'll find on most ARM
development boards, in the Raspberry Pi, and — important for us —
in the **QEMU `virt` machine** (the simulated machine type we boot
nonux on).

What ARM did with PL011 — and every other PrimeCell device — is
publish a *Technical Reference Manual* listing every register,
every bit, every signal. Anyone writing a driver follows the same
manual; QEMU's PL011 model follows the same manual on the device
side. So our driver, written from the manual, talks to QEMU's
implementation correctly.

The PL011 has a small **TX FIFO** (around 16 bytes) and a small
**RX FIFO** (also around 16 bytes). Software writes outgoing bytes
into the TX FIFO; the chip clocks them out on the wire one at a
time. Incoming bytes from the wire fill up the RX FIFO; software
reads them out as they arrive.

When the RX FIFO has at least one byte to read, the chip can raise
an IRQ — that's how we know "the user typed a key" without
constantly polling.

---

## How software talks to hardware: MMIO

Before we look at the driver, we need to understand how the CPU
gives instructions to a chip like the PL011. This is the same
mechanism every device driver in the kernel uses, so it's worth
spending a moment on.

### The "magic addresses" trick

On most modern systems, **RAM and devices share the same address
space.** The CPU issues a load or a store against an address;
depending on which range the address falls into, either a memory
chip or a device responds. Every device sits at some fixed range,
chosen by whoever designed the board (or in our case, whoever
designed the QEMU `virt` machine).

When the CPU executes a load or store *targeted at a device's
address*, the load or store doesn't go to RAM. It goes to the
device. The device sees "you stored byte 0x41 at offset 0 in my
range" and acts accordingly — for the PL011, that means "send the
character `A` out the TX wire".

This trick is called **memory-mapped I/O**, or **MMIO** for short.
Talking to a device looks like talking to memory:

```c
*(volatile uint32_t *)0x09000000 = 'A';   /* sends 'A' on the UART */
```

The `0x09000000` here is the PL011's base address on the QEMU
`virt` machine. The first 4-byte register at that address is the
*Data Register* — write a byte there, the chip sends it.

> **Side note: the alternative.** Older x86 chips have a *separate*
> address space for I/O, reached with special instructions like
> `in` and `out`. Modern systems mostly use MMIO instead, because
> it's simpler and uniform — one address space, one set of
> instructions. ARM has only ever had MMIO.

### The QEMU `virt` machine memory map

The QEMU `virt` machine puts each device at a fixed address. The
parts that matter for now:

| Address      | What's there                         |
|--------------|--------------------------------------|
| `0x08000000` | GIC distributor (the IRQ controller) |
| `0x08010000` | GIC CPU interface                    |
| `0x09000000` | **PL011 UART**                       |
| `0x40000000` | Start of RAM                         |
| `0x40080000` | Where our kernel image is loaded     |

Notice how the UART (`0x09000000`) sits *below* RAM (`0x40000000`).
That's normal — devices and RAM share one big number line, and
each gets a chunk. As long as our linker script (chapter 1) keeps
RAM mappings out of the device range, there's no conflict.

We hardcode `0x09000000` in the driver. On real hardware we'd
read it from the device tree blob (see chapter 1 for what a DTB
is); for QEMU we just know the number from the QEMU source.

### Why `volatile` matters

Look at the line in `printf.c` that tells the C compiler about the
UART:

```c
static volatile uint8_t *const uart = (volatile uint8_t *)UART_BASE;
```

The `volatile` here is critical. Without it, the C compiler is
free to assume that two consecutive writes to the same address can
be merged (since "the value is the same"), or that a load whose
result isn't used can be skipped. Both are normal compiler
optimizations and both are correct for *memory*.

For *MMIO*, both are wrong. Two writes to the same UART register
are very different from one — each write sends another byte. A
"useless" load might be reading a status register that *clears as
a side effect of being read*. The compiler doesn't know any of
that; `volatile` tells it "treat every access as if it has
side effects, because it does."

Whenever you see `volatile` in kernel code, the question to ask
is: "what hidden side effect is this hiding?". For MMIO the
answer is "talking to a device".

### One more thing: 32-bit access

Look closer:

```c
*(volatile uint32_t *)(uart + UART_FR) & UART_FR_TXFF
```

The pointer `uart` is declared as `uint8_t *` so we can do
byte-arithmetic with offsets like `UART_FR` (a small integer
constant). But every actual *access* casts to `uint32_t *` —
because PL011 registers are 32 bits wide, and the chip expects to
see a 32-bit transaction on the bus. A naive 8-bit access would
either fail outright, return zero on the unread bytes, or trigger
an alignment fault depending on the CPU.

Many MMIO drivers have bugs hiding behind exactly this: somebody
typed `uint8_t` where they should have typed `uint32_t`, and on
some chips it works by accident, but on others it doesn't. The
PL011 manual specifies 32-bit access; we follow the manual.

---

## TX (output): kprintf and uart_putc

With MMIO and `volatile` understood, the TX side of the driver is
short and clear. Here is the whole low-level write path from
[`core/lib/printf.c`](../core/lib/printf.c):

```c
#define UART_BASE    0x09000000UL
#define UART_DR      0x000          /* Data Register */
#define UART_FR      0x018          /* Flag Register */
#define UART_FR_TXFF (1 << 5)       /* Transmit FIFO full */

static volatile uint8_t *const uart = (volatile uint8_t *)UART_BASE;

void uart_putc(char c)
{
    /* Wait until TX FIFO is not full */
    while (*(volatile uint32_t *)(uart + UART_FR) & UART_FR_TXFF)
        ;
    *(volatile uint32_t *)(uart + UART_DR) = c;
}
```

Three things are happening:

1. **Read the Flag Register.** Address `UART_BASE + 0x018`. Bit 5
   (`TXFF` — TX FIFO full) tells us whether the TX FIFO has room
   for a new byte.
2. **Spin until there's room.** While that bit is set, the FIFO is
   full; we just keep re-reading the flag. The CPU does nothing
   else during this loop. For a 115200-baud UART that's
   microseconds at most — the FIFO drains as the chip clocks out
   the bytes already in it.
3. **Write the byte.** Once `TXFF` clears, store the character
   into the Data Register at `UART_BASE + 0x000`. The chip puts it
   at the back of its TX FIFO; eventually it goes out the wire and
   shows up on QEMU's terminal.

That's the entire output path to the hardware. **Twelve lines of C.**

### The `uart_init` no-op

You might have noticed in `boot_main` (chapter 1) the very first
line is `uart_init()`. Here's what `uart_init` actually does in
nonux:

```c
void uart_init(void)
{
    /* QEMU's PL011 works out of the box — no init needed for basic TX.
     * Real hardware would set baud rate, enable FIFOs, etc. */
}
```

Nothing. It's a no-op. Why?

QEMU's PL011 model is a software simulation. It's already
configured by the time the guest kernel starts, and there's no real
serial line, so things like baud rate don't matter — bytes
written to the Data Register show up in QEMU's terminal as fast as
QEMU can deliver them.

On **real hardware**, `uart_init` would have a lot more to do:

- Set the baud rate by writing to the `IBRD` (integer divisor) and
  `FBRD` (fractional divisor) registers. The right numbers depend
  on the UART's input clock, which depends on the chip.
- Set the line format — number of data bits, parity, stop bits —
  in `LCRH` (line control). Most setups are 8 data bits, no parity,
  1 stop bit, written as the bit pattern `0x70` in `LCRH`.
- Enable the TX FIFO and the receiver in `CR` (control).

We *do* eventually program some of these registers — but only on
the RX side, because RX needs interrupts to be set up before we
can use it. We'll see that in the next section. For TX, QEMU's
defaults are good enough.

### The `'\n'` translation

Look at `uart_puts`, the function that prints a whole string:

```c
void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}
```

It walks the string, calling `uart_putc` per byte. But before
emitting a `'\n'`, it first emits a `'\r'`. Why?

On any system with a line-discipline driver — which means every
hosted Linux program — there's a piece of kernel code that
translates a bare LF (`\n`) into the pair CR+LF (`\r\n`) before
the bytes hit a terminal. This is what the `ONLCR` flag in the
**POSIX** `termios` settings does, and it's on by default.

A bare-metal kernel writing directly to a UART has no such layer.
A terminal in **raw mode** treats the two characters very
differently:

- **`\n` (LF, line feed)** moves the cursor down one line — but
  not to the left.
- **`\r` (CR, carriage return)** moves the cursor back to the
  start of the line.

If you only send `\n`, you get this staircase effect:

```
hello,
       world
              again
```

If you send `\r\n` instead, you get what you'd expect:

```
hello,
world
again
```

So `uart_puts` synthesizes the `\r` before each `\n` itself. Same
trick lives inside `kprintf`'s main loop — every plain `\n`
character in a format string gets a `\r` emitted before it.

> **Side note.** Why is this the kernel's job at all? Because in
> nonux there is no "line discipline" yet. A more grown-up kernel
> would have one (a piece of code that turns `\n` into `\r\n` for
> output and turns Backspace into "erase one character" for input).
> v1 nonux skips it; the few places that emit `\n` add the `\r`
> by hand. busybox's shell handles its own line editing in *raw*
> mode anyway, so the missing line discipline mostly doesn't show.

### kprintf: the format-string parser

That brings us to `kprintf`. It's a hand-rolled clone of a tiny
subset of C's `printf` — enough to print the kinds of things a
kernel logs. The full source is in
[`core/lib/printf.c`](../core/lib/printf.c); here's its structure:

```c
void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n') uart_putc('\r');
            uart_putc(*fmt++);
            continue;
        }

        fmt++;            /* skip '%' */
        /* parse '0' pad and width */
        /* dispatch on conversion: s, d, u, x, p, c, l, % */
    }
    va_end(ap);
}
```

The big idea is **stream the output character by character**:

- For a plain character, just send it (with the `\n` → `\r\n` fix).
- For a `%`, look at what follows, pull the next argument with
  `va_arg`, format it, and send those characters.

The supported conversions are `%s`, `%d`, `%u`, `%x`, `%p`, `%c`,
`%lu`, `%lx`, `%ld`, and `%%`. There's a small flair: a leading `0`
in the width (e.g. `%08x`) means pad with zeros instead of spaces.

The number-to-text conversion is the kind of thing every C student
writes once:

```c
static void print_uint(uint64_t val, int base, int pad_width, char pad_char)
{
    char buf[20];
    int i = 0;

    if (val == 0) buf[i++] = '0';
    else while (val > 0) {
        int digit = val % base;
        buf[i++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
        val /= base;
    }

    /* pad on the left to the requested width */
    for (int j = i; j < pad_width; j++) uart_putc(pad_char);

    /* digits were produced low-to-high, print them in reverse */
    while (i > 0) uart_putc(buf[--i]);
}
```

Two details worth noticing:

- **`base` is a parameter.** That's what makes the same function
  serve `%u` (base 10), `%x` (base 16), and the inside of `%p`
  (also base 16, but always 16 wide and `0x`-prefixed).
- **Digits come out backwards.** Dividing by the base peels the
  *least* significant digit first, so the digits land in `buf[]`
  in reverse order. The final `while` walks them out forward.

Compared to the C library's `printf`, `kprintf` is missing a lot:
floating-point, `%g`, `%e`, `%n`, locale handling, signed-flag
toggling, and so on. None of that matters in a kernel — kernels
print integers, hex addresses, and strings. Keeping the
implementation tiny means it can run **before the heap is set up**,
**before any locks are acquired**, and **inside an IRQ handler if
needed**. None of those are true for a full `printf`.

The whole driver — `uart_init`, `uart_putc`, `uart_puts`, `kprintf`
with its formatter — is **165 lines** and is **the only thing
running** between `boot_main`'s first call and any text appearing
on screen.

### The full TX path

Putting it together, here's what happens when you call
`kprintf("hi\n")`:

```
kprintf("hi\n")              (your call)
    │
    ├─ for 'h': uart_putc('h')
    ├─ for 'i': uart_putc('i')
    └─ for '\n': uart_putc('\r'), then uart_putc('\n')
                 │
                 ▼
             uart_putc(c)
                 │
                 ├─ spin until *(uart + FR) & TXFF is clear
                 │
                 └─ *(uart + DR) = c
                                       │
                                       ▼
                                ┌──────────────────────┐
                                │  PL011 chip / QEMU   │
                                │  pushes byte to TX   │
                                │  FIFO, clocks out,   │
                                │  forwards to QEMU's  │
                                │  terminal window     │
                                └──────────────────────┘
```

That's the whole story for output. No queues we have to manage, no
threads, no allocations, no interrupts — just a synchronous spin
on a status flag and a single write per byte.

---

## RX (input): typing a key

Output is the easy direction. Input is more interesting because
the kernel can't *make* a byte arrive; it has to react when one
does. The shape of this problem comes up everywhere in kernel work,
so it's worth seeing it in detail on a small example like the UART.

### What happens on the wire

The TX side is "we want to send something, when can we?". The RX
side is the reverse: **somebody else wants to send us something,
how do we hear about it?**

Without help from the hardware, we'd have to keep polling — read
the RX status flag in a tight loop, see if a byte arrived, repeat.
That works (it's how some embedded systems do it) but it pegs the
CPU at 100% even when nothing is happening, and you lose bytes
between polls if they arrive too fast.

The PL011, like most reasonable hardware, supports **interrupt-driven
RX** instead. The deal goes like this:

1. We tell the chip "raise an IRQ when at least one byte arrives in
   the RX FIFO".
2. We tell the GIC (the system's interrupt controller) "deliver
   that IRQ to the CPU".
3. We wait. The kernel can do other things, sleep, anything.
4. Some bytes arrive on the RX wire. The PL011 puts them in its
   RX FIFO and signals the GIC.
5. The GIC interrupts the CPU. The CPU jumps to the kernel's IRQ
   handler. The handler runs our **ISR** for IRQ 33 (the PL011's
   IRQ number on the QEMU `virt` machine).
6. The ISR reads bytes out of the RX FIFO and stuffs them somewhere
   the rest of the kernel can find them.
7. The ISR clears the IRQ at the chip. The CPU resumes whatever it
   was doing.

The "somewhere" in step 6 is a **byte ring buffer**. Whoever
eventually wants to read input — busybox's shell, a kernel test —
reads bytes out of that ring, blocking if the ring is empty.

That separation of producer (the ISR) from consumer (someone
calling `read`) is the central pattern of input handling. It also
means writing the right code is harder than it looks, because the
ISR can fire at literally any moment. We'll come back to the
concurrency in a bit.

### Setting up the IRQ — `nx_console_init`

[`framework/console.c`](../framework/console.c) holds all of this.
Two short helpers it uses everywhere are `uart_rd(off)` and
`uart_wr(off, val)` — they're one-line wrappers around the same
`*(volatile uint32_t *)(uart + off)` cast we used in the TX
driver, just typed for readability. Whenever you see
`uart_rd(...)` or `uart_wr(...)`, mentally substitute the
volatile load or store from the MMIO section.

Here is the part that arms the hardware:

```c
#define UART_BASE   0x09000000UL
#define UART_IFLS   0x034   /* Interrupt FIFO Level Select */
#define UART_IMSC   0x038   /* Interrupt Mask Set/Clear */
#define UART_IMSC_RXIM (1U << 4)   /* RX-interrupt mask */
#define UART_IMSC_RTIM (1U << 6)   /* RX-timeout-interrupt mask */

#define PL011_IRQ 33   /* QEMU virt: SPI 1 = 32 + 1 */

void nx_console_init(void)
{
    /* IRQ when at least 1 byte is in the FIFO (1/8 full) */
    uart_wr(UART_IFLS, 0);

    /* Unmask receive + receive-timeout */
    uart_wr(UART_IMSC, UART_IMSC_RXIM | UART_IMSC_RTIM);

    if (irq_register(PL011_IRQ, nx_console_rx_isr, 0) != 0) {
        kprintf("[console] irq_register(%u) failed — RX disabled\n",
                (unsigned)PL011_IRQ);
        return;
    }
    gic_enable(PL011_IRQ);
    kprintf("[console] PL011 RX wired (irq=%u)\n", (unsigned)PL011_IRQ);
}
```

Five things happen here:

1. **`UART_IFLS = 0`** — set the RX FIFO interrupt level to "1/8
   full". On a 16-byte FIFO, that's 2 bytes — but importantly,
   the chip *also* fires a separate "receive timeout" IRQ (the
   `RTIM` bit) when the FIFO has held some bytes for a short while
   without crossing the threshold. Together they mean "you'll get
   an IRQ for any RX activity, even slow typing one key at a time".
2. **`UART_IMSC = RXIM | RTIM`** — unmask the two RX interrupts at
   the chip. The chip can now raise them.
3. **`irq_register(PL011_IRQ, nx_console_rx_isr, 0)`** — tell the
   kernel's per-IRQ dispatch table "if IRQ 33 fires, call the
   function `nx_console_rx_isr`".
4. **`gic_enable(PL011_IRQ)`** — tell the GIC (the interrupt
   controller) to deliver IRQ 33 to the CPU.
5. **A confirmation `kprintf`** — so the boot log says
   `[console] PL011 RX wired (irq=33)` if we got that far.

Note that the IRQ number 33 isn't magic. On the GICv2 controller in
the QEMU `virt` machine, IRQs 0–31 are reserved for per-CPU
private interrupts; "shared peripheral interrupts" (SPIs) start at
32. The PL011 happens to be SPI #1, which means `32 + 1 = 33`.
We just looked that up in QEMU's source.

[Chapter 3 (Exceptions, the GIC, and IRQs)](03-exceptions-gic-and-irqs.md)
walks `irq_register` and `gic_enable` under the covers. For
this chapter, treat them as the two switches you need to flip
to wire an IRQ from a device to a function in our code.

### The ISR — `nx_console_rx_isr`

When IRQ 33 fires, the kernel's general IRQ vector eventually
dispatches into this function:

```c
static void nx_console_rx_isr(void *data)
{
    (void)data;
    int wake = 0;
    while (!(uart_rd(UART_FR) & UART_FR_RXFE)) {
        char c = (char)(uart_rd(UART_DR) & 0xFF);
        if (c == 0x03) {              /* Ctrl-C */
            __atomic_store_n(&g_intr_pending, 1, __ATOMIC_RELEASE);
            continue;
        }
        if (c == 0x04) {              /* Ctrl-D */
            __atomic_store_n(&g_eof_pending, 1, __ATOMIC_RELEASE);
            wake = 1;
            continue;
        }
        if (rx_push_one(c)) wake = 1;
    }
    /* Acknowledge the IRQ at the chip */
    uart_wr(UART_ICR, UART_IMSC_RXIM | UART_IMSC_RTIM);

    if (wake) nx_pollset_wake_all(&g_console_pollset_listeners);
}
```

The loop:

1. Read `FR` (Flag Register). Bit 4 is `RXFE` — RX FIFO empty. As
   long as it's *not* set, there's at least one byte waiting.
2. Read `DR` (Data Register). That returns the next byte from the
   FIFO and removes it.
3. Look at the byte. Two values are special:
   - **`0x03` (Ctrl-C)**: don't put it in the byte ring. Set a flag
     `g_intr_pending` instead — the kernel will pick it up later
     and post a **signal** (`SIGTERM`, "please terminate") to user
     processes. Full handler-driven signal delivery is a story
     for a later chapter.
   - **`0x04` (Ctrl-D)**: same idea. Set `g_eof_pending`. The next
     `read` on stdin will return 0 (EOF) instead of blocking.
4. Otherwise, **push the byte onto the ring** with `rx_push_one`.
5. Repeat until the FIFO is empty.

Then:

- **Clear the IRQ at the chip.** Writing `RXIM | RTIM` to the
  `ICR` (Interrupt Clear Register) tells the PL011 "I've handled
  these — stop signaling the GIC". Without this, the IRQ would
  fire again instantly.
- **Wake up anyone who's waiting.** If at least one byte was
  pushed (or EOF arrived), notify a list of waiters via
  `nx_pollset_wake_all`. We'll see who they are when we look at
  the read side.

Two design rules to flag, because they apply to almost every ISR
you'll write:

- **The ISR drains the FIFO completely.** It doesn't stop after
  one byte. If we left bytes behind, the chip would IRQ again
  immediately for the same condition, and we'd just be doing twice
  the work for the same outcome. Drain everything you have, then
  ack.
- **The ISR doesn't block.** No locks held longer than they need
  to be, no calls into long-running code, no allocations. The CPU
  is paused on this handler — the longer it sits here, the more
  user code has to wait.

### The ring buffer

Here are the ring's pieces:

```c
#define RX_RING_SIZE 256

static char            g_rx_buf[RX_RING_SIZE];
static _Atomic size_t  g_rx_head;
static _Atomic size_t  g_rx_tail;
```

Three things: a fixed array, a `head` index (the producer writes
here next), and a `tail` index (the consumer reads from here next).
Both indices grow forever from the consumer's point of view; we
mod by `RX_RING_SIZE` (which is a power of two, hence the cheap
`& (RX_RING_SIZE - 1)`) when actually indexing the array.

The producer (`rx_push_one`, called from the ISR):

```c
static inline int rx_push_one(char c)
{
    size_t h = __atomic_load_n(&g_rx_head, __ATOMIC_RELAXED);
    size_t t = __atomic_load_n(&g_rx_tail, __ATOMIC_ACQUIRE);
    size_t next = (h + 1) & (RX_RING_SIZE - 1);
    if (next == t) return 0;   /* full — drop */
    g_rx_buf[h] = c;
    __atomic_store_n(&g_rx_head, next, __ATOMIC_RELEASE);
    return 1;
}
```

The consumer (`rx_pop_one`, called from the read syscall):

```c
static inline int rx_pop_one(char *out)
{
    size_t t = __atomic_load_n(&g_rx_tail, __ATOMIC_RELAXED);
    size_t h = __atomic_load_n(&g_rx_head, __ATOMIC_ACQUIRE);
    if (h == t) return 0;
    *out = g_rx_buf[t];
    __atomic_store_n(&g_rx_tail, (t + 1) & (RX_RING_SIZE - 1),
                     __ATOMIC_RELEASE);
    return 1;
}
```

Empty when `head == tail`. Full when `(head + 1) % SIZE == tail`
— note this means we leave one slot unused, so we can tell empty
and full apart by inspecting head and tail alone.

The `_Atomic` qualifiers and the `__atomic_*` builtins matter
because the producer (ISR context) and consumer (a **kthread** or
a user task) really do run "at the same time" from a
memory-ordering perspective. The producer must:

- Write the byte into `g_rx_buf[h]` *before* publishing the new
  `head`.
- Use a release store on `head` so the consumer, seeing the new
  head value, also sees the byte (because of the matching acquire).

Symmetrically the consumer reads `head` with acquire, reads the
byte, then publishes the new `tail` with release.

> **Why this works without locks.** A lock-free ring buffer is
> safe specifically when there is at most one producer and at most
> one consumer. In nonux, the ISR is the sole producer (interrupts
> for IRQ 33 don't stack on a single CPU) and the read syscall is
> the sole consumer (the kernel only allows one task at a time
> waiting on the console). If we ever had two producers, the
> push code would race; we'd need a different structure. So this
> simple version doesn't generalize, but it's perfect for the
> case it covers.

### The reader: `nx_console_read`

Finally, the function that pulls bytes back out — called from the
syscall path when an EL0 program does `read(0, buf, len)`. The
kernel build (we'll skip the host-only stub) looks like this:

```c
int nx_console_read(void *buf, size_t cap)
{
    if (cap == 0) return 0;
    if (!buf)     return -1;
    char *out = (char *)buf;
    size_t got = 0;

    struct nx_waitq             rx_wq;
    struct nx_pollset_listener  listener;
    nx_waitq_init(&rx_wq);
    nx_pollset_listener_init(&listener, &rx_wq);
    nx_console_register_pollset(&listener);

    while (got < cap) {
        char c;
        if (rx_pop_one(&c)) {
            out[got++] = c;
            continue;
        }
        if (got > 0) break;        /* return what we have */
        if (__atomic_exchange_n(&g_eof_pending, 0, __ATOMIC_ACQ_REL)) {
            nx_console_unregister_pollset(&listener);
            return 0;              /* Ctrl-D — POSIX read EOF */
        }
        nx_waitq_wait_unless(&rx_wq, 0,
                             console_read_ready_pred, NULL);
    }
    nx_console_unregister_pollset(&listener);
    return (int)got;
}
```

The block at the top — `nx_waitq_init`, `nx_pollset_listener_init`,
`nx_console_register_pollset` — is the read side asking the ISR
"please wake me when you push a byte". Symmetrically, every exit
from the function calls `nx_console_unregister_pollset` to take
the listener off the wake list. Inside the loop,
`nx_waitq_wait_unless`'s third argument, `console_read_ready_pred`,
is a one-line predicate that returns true when the ring has at
least one byte (or EOF is queued); the wait checks it inside its
own critical section to plug a lost-wakeup race.

Strip that bookkeeping and the loop says:

- Try to pop a byte from the ring.
- If the buffer already has some bytes and the ring's empty, return
  what we have. A short read is fine.
- If the ring's empty and a Ctrl-D was injected, return 0 (EOF).
- Otherwise, **block** on a wait queue until the ISR signals "a
  byte just arrived".

The wait-queue and pollset machinery — what `nx_waitq_wait_unless`
actually does, how the scheduler suspends and resumes the calling
task, how the listener list connects producers and consumers — gets
covered in its own chapters later. The thing to take away here is
the **shape** of the I/O loop:

- **Producer side (ISR):** push a byte, wake any waiters.
- **Consumer side (read):** pop bytes if present, otherwise wait.

That same shape — non-blocking try, fall back to wait if empty,
wake on the producer side — runs through every blocking I/O path
in nonux. The console is the simplest example of it.

### The full RX path

Putting it together for "user types one key in the QEMU terminal":

```
QEMU terminal: user presses 'a'
        │
        ▼
QEMU pushes 'a' into PL011 RX FIFO,
asserts SPI #1 → IRQ 33 to GIC
        │
        ▼
GIC delivers IRQ 33 to the CPU
        │
        ▼
CPU jumps to EL1 IRQ vector (core/cpu/exception.S)
        │
        ▼
irq_dispatch() → nx_console_rx_isr(NULL)
        │
        ├── drain FIFO via uart_rd(DR) loop
        │       └── for 'a': rx_push_one('a')
        ├── uart_wr(ICR, RXIM|RTIM)   /* ack at chip */
        └── nx_pollset_wake_all(...)
        │
        ▼ (CPU returns from IRQ)
        ▼ scheduler picks up a task waiting on console RX
        │
        ▼
nx_console_read(buf, cap) loops:
  - rx_pop_one(&c) succeeds → buf[0] = 'a'
  - cap satisfied or ring empty → return 1
        │
        ▼
sys_read returns 1 to EL0
        │
        ▼
busybox sees the keystroke
```

---

## Two paths to the same UART

We started the chapter saying the console has **two paths** into
the PL011: the direct path used by the kernel itself, and the
component path used by userspace. We've fully walked the direct
path. Now let's say two things about the component path so it's
not surprising when you see it in the source.

The component path's job is to make the UART look like a normal
character device — something a user program can `write` to and
`read` from with regular file descriptors:

```c
write(1, "hello\n", 6);     /* in an EL0 program */
read(0, buf, 16);
```

For these to work, the kernel needs:

- A **slot** in the kernel's composition called `char_device.serial`,
  filled by some component.
- A component with a `write` and `read` op that does the actual
  byte work.

The component that fills the slot is **`uart_pl011`**, the one in
[`components/uart_pl011/uart_pl011.c`](../components/uart_pl011/uart_pl011.c).
Its core, with framework noise stripped, is just this:

```c
static int64_t uart_pl011_write(void *self, const void *buf, size_t len)
{
    return (int64_t)nx_console_write(buf, len);
}

static int64_t uart_pl011_read(void *self, uint32_t id, void *buf, size_t cap)
{
    return (int64_t)nx_console_read_nonblocking(buf, cap);
}
```

That's the whole component, on the byte-handling side. **Both
calls eventually go through `uart_putc` (output) or the same RX
ring (input)** — exactly the path we just walked through.
`nx_console_write` is a tiny function in `framework/console.c`
that loops `uart_putc` over the buffer; `nx_console_read_nonblocking`
is the same pop-from-the-ring code as `nx_console_read` but it
returns immediately (with `NX_EAGAIN`) when the ring is empty
instead of blocking. The component is not a *different* driver;
it's a thin wrapper that exposes the existing driver through a
standard interface so the syscall layer can find it.

Why bother? Three reasons:

1. **The slot is swappable.** If we wanted to send all kernel
   output over a network instead of the UART, we could write a
   `char_device` component called `net_console` and have the
   slot point at it. None of the syscall code changes.
2. **It separates the userspace contract from the device.** EL0
   programs see "write to fd 1". The kernel translates that into
   "the component currently filling `char_device.serial`, please
   write these bytes". The chain of indirection is exactly what
   makes nonux a *composable* kernel.
3. **It registers as a component.** The bottom of `uart_pl011.c`
   has the line:

   ```c
   NX_COMPONENT_REGISTER_NO_DEPS_IFACE(uart_pl011,
                                       struct uart_pl011_state,
                                       &uart_pl011_ops,
                                       &uart_pl011_char_device_ops);
   ```

   Chapter 1 already met this kind of macro: it emits a `struct`
   into the `nx_components` linker section, and at boot the
   framework walks that section and finds every component
   automatically. The kernel knows it has a UART driver because
   the linker put it on the list.

The full story of slots, components, the framework, and how
`write(1, "hi", 2)` actually routes into `uart_pl011_write` will
get covered in later chapters. For now: there's one UART, one
hardware register, and two callers — `kprintf` (kernel-side) and
`uart_pl011_write` (userspace-side, via the syscall layer). Both
end up at the same `uart_putc` and the same MMIO store.

---

## A few extra things to know

- **`kprintf` is single-CPU-friendly, not multi-CPU-safe.** Two
  CPUs calling `kprintf` at the same time would interleave their
  output character by character. nonux currently runs on one CPU,
  so it doesn't matter. Real multi-CPU kernels add a per-console
  spinlock around output, or a per-CPU buffer that gets flushed in
  a controlled way.

- **`kprintf` is also IRQ-context-safe by being trivial.**
  Anything more elaborate — a heap allocation for a message
  buffer, a lock, blocking on a semaphore — would be wrong inside
  an ISR. Keeping `kprintf` as "format inline, busy-wait on the
  TX FIFO, store one byte" means the ISR can call it. We use this
  liberally; almost every kernel error message comes from inside
  some interrupt or fault handler.

- **The PL011's TX FIFO is a smoothing layer.** When you call
  `kprintf("[boot] hello\n")`, the chip is happily clocking out
  earlier bytes while we're still queueing later ones. The FIFO
  decouples the CPU's burst pattern from the wire's steady drip.
  When the FIFO fills up, our `while (... TXFF)` loop spins; when
  there's space, we move on.

- **No encoding awareness.** The UART deals in raw bytes. ASCII
  characters (codepoints below 128) are one byte each and pass
  through fine. UTF-8 characters above 128 are encoded as 2–4
  bytes; if you `kprintf("héllo")`, those bytes go out, and
  whether you see "héllo" depends on whether the receiving
  terminal is set to interpret UTF-8. nonux's internal strings are
  ASCII-only, so this hasn't bitten us.

- **`uart_putc` blocks the CPU.** If for some reason the TX FIFO
  stayed full forever (a stuck device, a misconfigured chip), our
  `while ((... ) & TXFF) ;` loop would never return — and the
  whole kernel hangs there. On QEMU this never happens because
  the FIFO drains as fast as QEMU's terminal accepts bytes. On
  real hardware, drivers usually add a small timeout to be safe.

- **Why the RX side has a ring but the TX side doesn't.** TX is
  driven *by us*: when we want to send something, we know it. We
  can wait on the FIFO directly, byte by byte. RX is driven *by
  the outside world*: bytes arrive when they arrive. We need
  somewhere to put them between "the ISR caught them" and "a task
  decides to read". That somewhere is the ring.

---

## Where to read more

- [`core/lib/printf.c`](../core/lib/printf.c) — full source of the
  early UART driver and `kprintf`.
- [`framework/console.c`](../framework/console.c) — the RX ring,
  the ISR, the read path, and the Ctrl-C / Ctrl-D side channels.
- [`components/uart_pl011/uart_pl011.c`](../components/uart_pl011/uart_pl011.c) —
  the same UART exposed as a `char_device` component for userspace.
- [`../core/README.md`](../core/README.md) — what other things in
  `core/` exist (mmu, pmm, irq, timer, sched), in the order
  `boot_main` brings them up.
- [Chapter 1 §"What the CPU is doing the moment our code starts"](01-boot-and-linker.md#what-the-cpu-is-doing-the-moment-our-code-starts)
  — for context on why the kernel is allowed to access MMIO at all
  (it runs at EL1, so privileged loads/stores work).
- ARM PrimeCell UART (PL011) Technical Reference Manual
  (`infocenter.arm.com`, document DDI0183) — the canonical source
  for every PL011 register, every bit, every timing detail. It's
  the document our driver was written from. Heavy but
  authoritative; come back to it once the basics in this chapter
  feel comfortable.
- [QEMU's `virt` machine source](https://gitlab.com/qemu-project/qemu)
  (`hw/arm/virt.c`) — has the table of fixed device addresses
  including the PL011 at `0x09000000` and IRQ 33. The actual
  numbers in our driver come from there.
- ARM Generic Interrupt Controller v2 Architecture Specification
  — the manual for the GIC, the chip that delivers IRQ 33 to the
  CPU. The reference for [chapter 3](03-exceptions-gic-and-irqs.md);
  not needed for this chapter.
