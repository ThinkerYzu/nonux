#include "exception.h"
#include "core/lib/lib.h"
#include "core/irq/irq.h"
#include "framework/syscall.h"
#include "framework/process.h"

extern char vectors[];

void vectors_install(void)
{
    asm volatile("msr vbar_el1, %0" :: "r"((uint64_t)vectors) : "memory");
    asm volatile("isb" ::: "memory");
}

static void halt_forever(void)
{
    for (;;) asm volatile("wfe");
}

/* ESR_EL1.EC values we care about — see ARMv8 ARM, "Exception Syndrome
 * Register, EL1".  EC sits in bits[31:26] of ESR_EL1; we right-shift 26
 * and mask to 6 bits before comparing.
 *
 * The "lower EL" variants (0x20, 0x24) tell us the fault came from EL0
 * (the only "lower" EL we have).  The "current EL" variants (0x21,
 * 0x25) mean the kernel itself faulted — those stay as panic.  EC=0x00
 * is "Unknown reason" and ambiguous; we read the saved SPSR.M bits
 * (`tf->pstate`) to decide whether the fault originated at EL0 or
 * EL1. */
#define ESR_EC_UNKNOWN          0x00u  /* undefined instruction etc. */
#define ESR_EC_SVC64            0x15u
#define ESR_EC_INST_ABORT_EL0   0x20u
#define ESR_EC_INST_ABORT_EL1   0x21u
#define ESR_EC_DATA_ABORT_EL0   0x24u
#define ESR_EC_DATA_ABORT_EL1   0x25u

/* Slice 7.6d.3a: signo values used by sched_check_resched's POSIX-
 * shaped `128 + signo` exit-code convention (matches NX_SIGKILL /
 * NX_SIGTERM in framework/syscall.h, but we hardcode the well-known
 * POSIX numbers here so a future signal renumbering doesn't silently
 * change exit codes). */
#define NX_FAULT_SIGSEGV  11
#define NX_FAULT_SIGILL    4

/* SPSR_EL1.M[3:0] = 0b0000 → EL0t (the only EL0 mode we use).  Any
 * other value means the fault was taken with the CPU at EL1 (M=0b0100
 * = EL1t, 0b0101 = EL1h, etc.) — kernel bug, panic. */
static int saved_pstate_was_el0(uint64_t pstate)
{
    return (pstate & 0xfu) == 0u;
}

/* Slice 7.6d.3a: convert an EL0 fault into a parent-observable process
 * exit.  nx_process_exit(128 + signo) sets the current process's state
 * to EXITED + parks the task in wfe.  Parent's wait() collects.
 *
 * Caller must guarantee the fault originated at EL0 (otherwise we'd be
 * exiting whatever process the kernel happened to be associated with
 * when it tripped, which is almost certainly a different bug to
 * diagnose).  This function does not return. */
static __attribute__((noreturn))
void deliver_el0_fault_signal(int signo, const char *reason,
                              uint64_t esr, uint64_t far, uint64_t pc)
{
    kprintf("\n[EXC] EL0 %s ESR=%lx FAR=%lx ELR=%lx -> exit %d\n",
            reason, esr, far, pc, 128 + signo);
    nx_process_exit(128 + signo);
    /* nx_process_exit doesn't return; halt as a safety net if it ever
     * grows a return path. */
    halt_forever();
}

void on_sync(struct trap_frame *tf)
{
    uint64_t esr, far;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    uint32_t ec = (uint32_t)((esr >> 26) & 0x3fu);

    if (ec == ESR_EC_SVC64) {
        nx_syscall_dispatch(tf);
        return;
    }

    asm volatile("mrs %0, far_el1" : "=r"(far));

    /* Slice 7.6d.3a: lower-EL faults become POSIX-shaped process exits
     * so a parent's wait() observes `128 + signo` and the kernel keeps
     * running.  Anything else (current-EL fault, EC we don't handle)
     * panics — those are kernel bugs, not user-program bugs, and a
     * silent exit would mask them. */
    switch (ec) {
    case ESR_EC_INST_ABORT_EL0:
        deliver_el0_fault_signal(NX_FAULT_SIGSEGV, "inst_abort",
                                 esr, far, tf->pc);
        /* unreachable */
    case ESR_EC_DATA_ABORT_EL0:
        deliver_el0_fault_signal(NX_FAULT_SIGSEGV, "data_abort",
                                 esr, far, tf->pc);
        /* unreachable */
    case ESR_EC_UNKNOWN:
        /* Could be from either EL — check SPSR.  Undef from EL0 →
         * SIGILL.  From EL1 → kernel bug, panic. */
        if (saved_pstate_was_el0(tf->pstate)) {
            deliver_el0_fault_signal(NX_FAULT_SIGILL, "undef",
                                     esr, far, tf->pc);
            /* unreachable */
        }
        break;
    default:
        /* Other EC values: FP/SIMD trap (0x07), HVC/SMC (0x12/0x13/
         * 0x16/0x17), watchpoint (0x34/0x35), etc.  Defer those until
         * a real demo trips them; for now they fall through to panic
         * so we notice if anything new shows up. */
        break;
    }

    kprintf("\n[EXC] sync  ESR=%lx FAR=%lx ELR=%lx SPSR=%lx\n",
            esr, far, tf->pc, tf->pstate);
    halt_forever();
}

void on_irq(struct trap_frame *tf)
{
    (void)tf;
    irq_dispatch();
}

void on_fiq(struct trap_frame *tf)
{
    (void)tf;
    kprintf("\n[EXC] unexpected FIQ\n");
    halt_forever();
}

void on_serror(struct trap_frame *tf)
{
    (void)tf;
    kprintf("\n[EXC] SError\n");
    halt_forever();
}

void on_unimpl(struct trap_frame *tf)
{
    kprintf("\n[EXC] unimplemented vector ELR=%lx SPSR=%lx\n",
            tf->pc, tf->pstate);
    halt_forever();
}
