# core/

Platform-level kernel primitives. Everything in here is hardware-facing
and architecture-specific (ARM64); nothing above this layer talks directly
to hardware.

## Subdirectories

### `boot/`

ARM64 reset-vector entry and very-early C init.

| File | Role |
|------|------|
| `start.S` | Reset vector. Sets up EL1 stack, zeroes BSS, jumps to `boot_main()` in `boot.c`. |
| `boot.c` | First C code to run. Initialises GIC, MMU, PMM, timer, then calls `nx_framework_bootstrap()` to bring up the component graph. |
| `linker.ld` | Linker script. Places kernel image at `0x40080000`, defines the `nx_components` linker section consumed by `framework/bootstrap.c`. |

### `cpu/`

Context switching, exception vectors, and the EL0 entry path.

| File | Role |
|------|------|
| `vectors.S` | ARM64 exception vector table (VBAR_EL1 target). Dispatches sync exceptions, IRQs, FIQs, and SErrors for both EL1 and EL0 origins. |
| `exception.c/.h` | C-level exception handler. Routes EL0 sync exceptions to the syscall table or the fault handler; routes IRQs to `irq_dispatch()`. |
| `context.S` | `nx_context_switch(from, to)` — saves callee-saved registers + SP into `from`, restores them from `to`, returns on the new stack. |
| `el0_entry.S` | EL1→EL0 transition (`nx_el0_enter`). Sets up SPSR_EL1, ELR_EL1, SP_EL0, and ERET to drop into userspace at the specified entry point. |
| `monotonic.h` | Inline helpers to read the ARM64 generic counter (`CNTPCT_EL0`) as a monotonic nanosecond timestamp. |

### `irq/`

Interrupt routing between the GIC hardware and kernel subsystems.

| File | Role |
|------|------|
| `gic.c` | GIC-400 (ARM Generic Interrupt Controller v2) driver. Initialises the distributor and CPU interface at the QEMU virt base address; enables/disables individual interrupt lines. |
| `irq.c/.h` | IRQ registration and dispatch. `irq_register(intid, handler, arg)` installs a handler; `irq_dispatch()` is called from the exception handler and fans out to the registered handler. |

### `lib/`

Freestanding utility library used throughout the kernel (no libc dependency).

| File | Role |
|------|------|
| `kheap.c/.h` | Kernel heap allocator. PMM-backed slab that provides `malloc`/`calloc`/`free` for the kernel build. The host build links against libc's allocator instead. |
| `printf.c` | Bare-metal `printf` / `vprintf`. Outputs over the UART (via `console_putchar`). Used for both debug output and ktest pass/fail markers. |
| `string.c` | Minimal string/memory functions: `memset`, `memcpy`, `memmove`, `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`. |
| `lib.h` | Common declarations shared across `core/`: `ARRAY_SIZE`, `MIN`/`MAX`, `container_of`, `offsetof`, `BUG_ON`. |
| `list.h` | Intrusive doubly-linked list (`struct nx_list_head`, `nx_list_add`, `nx_list_del`, `nx_list_for_each`). Used by the registry, wait-queue, and scheduler internals. |
| `mpsc.h` | Lock-free multi-producer single-consumer ring (Vyukov-style). Backing store for the async IPC inbox drained by `framework/dispatcher.c`. |

### `mmu/`

ARM64 virtual memory and page table management.

| File | Role |
|------|------|
| `mmu.c/.h` | MMU initialisation and per-process page table operations. Currently uses L1+L2 (2 MiB block) descriptors for both kernel (TTBR1_EL1) and user (TTBR0_EL1) address spaces. Phase 9 will add an L3 level for 4 KiB page granularity, VMAs, and COW fork. |

### `pmm/`

Physical memory manager.

| File | Role |
|------|------|
| `pmm.c/.h` | Bitmap-based physical page allocator. At boot, `boot.c` calls `pmm_init()` with the usable RAM range. Provides `pmm_alloc_page()` / `pmm_free_page()` and a range-reservation API used by the MMU to mark device memory. |

### `sched/`

Low-level task primitives. Policy lives in `components/sched_*`; mechanism lives here.

| File | Role |
|------|------|
| `task.c/.h` | `struct nx_task` and its lifecycle: `nx_task_create`, `nx_task_destroy`, `nx_task_switch`. Owns the per-task kernel stack and the current-task pointer. |
| `sched.c/.h` | Thin dispatch layer between the kernel and the active scheduler component. `sched_next()` calls into the component via `nx_slot_call_blocking`; `sched_yield()` is the preemption point. |
| `waitq.c/.h` | Wait-queue primitive. `nx_waitq_wait()` blocks the current task (`NX_TASK_BLOCKED`); `nx_waitq_wake_one()` / `nx_waitq_wake_all()` move waiters back to the run queue. Used by IPC pause/drain, ppoll, and pipe blocking. |

### `timer/`

ARM64 generic timer driver (the kernel's sole tick source).

| File | Role |
|------|------|
| `timer.c/.h` | Programmes `CNTV_CVAL_EL0` for the next tick; the timer IRQ handler calls `sched_tick()` to drive preemption. Also exposes `timer_pause()` / `timer_resume()` used by the recomposition orchestrator when swapping the scheduler component. |
