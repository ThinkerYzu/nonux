#include "core/lib/lib.h"
#include "core/mmu/mmu.h"
#include "core/pmm/pmm.h"
#include "core/cpu/exception.h"
#include "core/irq/irq.h"
#include "core/timer/timer.h"
#include "core/sched/sched.h"
#include "framework/bootstrap.h"
#include "framework/console.h"
#include "framework/process.h"
#include "framework/registry.h"

/*
 * boot_main — C entry point after start.S sets up EL1, stack, and BSS.
 *
 * Bring-up order:
 *   1. UART (low-level setup done in Phase 1).
 *   2. PMM over all RAM above __free_mem_start up to RAM_END.
 *   3. Exception vectors (VBAR_EL1).
 *   4. GIC distributor + CPU interface.
 *   5. ARM Generic Timer at 10 Hz.
 *   6. Framework bootstrap (slice 3.9a): walk the nx_components linker
 *      section, register slots + components from gen/slot_table.c,
 *      run init → enable in topo order, dump the composition.
 *   7. Unmask IRQ, park in wfi.  Tick handler prints once/sec.
 *
 * RAM_END is hardcoded to 0x80000000 (matches QEMU -m 1G).  Phase 5's DTB
 * parsing will replace this with the DTB-reported value.
 */

extern char __bss_start[];
extern char __bss_end[];
extern char __kernel_end[];
extern char __free_mem_start[];
extern char vectors[];

#define RAM_END 0x80000000UL

#ifdef NX_INIT_BUSYBOX
/*
 * Slice 7.6d.N.final.b — busybox-as-/init init runner.
 *
 * Production build path that turns kernel-busybox.bin into an
 * interactive shell.  The flow mirrors ktest_exec.c (slice 7.4c) but
 * has no test parent, no marker counter, and no dequeue cleanup —
 * the init kthread drops to EL0, the EL0 stub `execve`s /init
 * (= busybox via --busybox-init), and busybox `sh` runs forever.
 *
 * Bytes typed at the QEMU UART end up in the CONSOLE RX ring (slice
 * 7.6d.N.final.a) and bubble up through `read(0, ...)` from EL0; ash
 * drives its line editor against that.
 */
extern char __init_busybox_prog_start[];
extern char __init_busybox_prog_end[];

static struct nx_process *g_init_proc;

static void nx_init_busybox_kthread(void *arg)
{
    (void)arg;
    /* Copy the EL0 init stub into the new process's user-window
     * backing.  Same pattern as ktest_exec's exec_el0_kthread:
     * memcpy → cache maintenance → drop_to_el0 (one-way).  The stub
     * SVCs into NX_SYS_EXEC against /init with argv = {"sh", NULL}. */
    void *backing = mmu_address_space_user_backing(g_init_proc->ttbr0_root);
    size_t len = (size_t)(__init_busybox_prog_end - __init_busybox_prog_start);
    const char *src = __init_busybox_prog_start;
    char       *dp  = backing;
    for (size_t i = 0; i < len; i++) dp[i] = src[i];
    asm volatile ("dsb ish"  ::: "memory");
    asm volatile ("ic iallu" ::: "memory");
    asm volatile ("dsb ish"  ::: "memory");
    asm volatile ("isb");

    uint64_t base   = mmu_user_window_base();
    uint64_t size   = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    kprintf("[init] entering busybox sh at EL0\n");
    drop_to_el0(base, sp_el0);
}

static void nx_init_busybox_main(void)
{
    g_init_proc = nx_process_create("init");
    if (!g_init_proc) {
        kprintf("[init] nx_process_create failed — halting\n");
        for (;;) asm volatile ("wfi");
    }
    if (sched_spawn_kthread("init", nx_init_busybox_kthread, 0,
                            g_init_proc) == 0) {
        kprintf("[init] sched_spawn_kthread failed — halting\n");
        for (;;) asm volatile ("wfi");
    }
    /* Idle here forever; the scheduler will rotate through the init
     * process's task (and any of its descendants) preemptively. */
    for (;;) asm volatile ("wfi");
}
#endif /* NX_INIT_BUSYBOX */

void boot_main(void)
{
    uart_init();

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  nonux — composable microkernel\n");
    kprintf("  ARM64 / QEMU virt\n");
    kprintf("========================================\n");
    kprintf("\n");

    /* Slice 5.1: turn on the MMU as early as possible so the rest of
     * bring-up runs with D-cache + I-cache enabled and Normal-memory
     * semantics (unaligned access, weaker ordering).  VA = PA
     * everywhere we map — turning it on does not change any symbol
     * address, only how loads/stores / fetches behave. */
    mmu_init();
    kprintf("[mmu]  enabled (identity map: MMIO 0..1G Device, RAM 1..2G Normal)\n");

    kprintf("[boot] kernel loaded at 0x40080000 (QEMU's -kernel offset)\n");
    kprintf("[boot] BSS:  %p — %p\n",
            (uint64_t)__bss_start, (uint64_t)__bss_end);
    kprintf("[boot] kernel end:    %p\n", (uint64_t)__kernel_end);
    kprintf("[boot] free memory:   %p — %p\n",
            (uint64_t)__free_mem_start, (uint64_t)RAM_END);
    kprintf("\n");

    uintptr_t pmm_base = (uintptr_t)__free_mem_start;
    pmm_init(pmm_base, (size_t)(RAM_END - pmm_base));
    /* Slice 7.6d.2b: reserve the user-window VA range from PMM so kernel
     * data (kstacks, page tables, slab pages) never lands at a PA that
     * overlaps it.  Per-process TTBR0 has slot USER_WINDOW_INDEX..
     * overridden as user_block descriptors pointing at the process's
     * own user_pa, so EL1 access via identity-VA in that range would
     * alias to the wrong PA when running under a process's TTBR0 —
     * the failure mode is invisible until the kernel happens to access
     * its own stack/state via VA in the range, which is intermittent
     * for 2 MiB user windows and guaranteed once user_pa chunks reach
     * 8 MiB and routinely span the user-window PA range. */
    pmm_reserve_range((uintptr_t)mmu_user_window_base(),
                      (size_t)mmu_user_window_size());
    kprintf("[pmm]  total=%lu free=%lu pages (%lu KiB) [user-window reserved]\n",
            (uint64_t)pmm_total_count(),
            (uint64_t)pmm_free_count(),
            (uint64_t)pmm_free_count() * 4);

    vectors_install();
    kprintf("[cpu]  exception vectors installed at %p\n", (uint64_t)vectors);

    gic_init();
    kprintf("[gic]  distributor + CPU interface enabled\n");

    timer_init(10);

    /* Slice 7.6d.N.final.a: register the PL011 RX ISR + enable
     * IRQ 33 at the GIC.  Has to land after gic_init (so the GIC's
     * distributor + CPU interface are up) and before
     * irq_enable_local (so the first key press doesn't fire into a
     * masked vector). */
    nx_console_init();

    /* Phase 3 bring-up — walk nx_components, register every slot +
     * descriptor, run init / enable in topo order.  Any non-OK return
     * leaves the composition partially up; for now we just log and
     * carry on (slice 3.9b will own the rollback story). */
    int fw_rc = nx_framework_bootstrap();
    if (fw_rc == NX_OK) {
        static char snap_buf[4096];
        struct nx_graph_snapshot *snap = nx_graph_snapshot_take();
        if (snap) {
            int n = nx_graph_snapshot_to_json(snap, snap_buf, sizeof snap_buf);
            kprintf("[fw]   composition (gen=%lu, %lu slots, %lu components):\n",
                    (uint64_t)nx_graph_snapshot_generation(snap),
                    (uint64_t)nx_graph_snapshot_slot_count(snap),
                    (uint64_t)nx_graph_snapshot_component_count(snap));
            if (n > 0) uart_puts(snap_buf);
            uart_puts("\n");
            nx_graph_snapshot_put(snap);
        }
    } else {
        kprintf("[fw]   bootstrap failed (rc=%d)\n", fw_rc);
    }

    /* Slice 4.4: transition the boot context into the idle task and
     * enqueue it as the scheduler's runqueue fallback BEFORE enabling
     * IRQs.  Doing it in this order means the very first tick after
     * irq_enable_local can drive sched_tick/sched_check_resched
     * against a well-formed current.  No-op if sched_init hasn't
     * run (e.g. the composition doesn't wire up a scheduler slot). */
    sched_start();

    irq_enable_local();

#ifdef NX_KTEST
    /* When built with -DNX_KTEST, run the in-kernel test suite instead
     * of the idle loop.  ktest_main runs in the idle-task context
     * (TPIDR_EL1 now points at the core driver's idle_task); tests
     * can spawn kthreads that preempt ktest_main via timer ticks and
     * yield back cooperatively.  ktest_main exits via semihosting. */
    extern void ktest_main(void) __attribute__((noreturn));
    ktest_main();
#elif defined(NX_INIT_BUSYBOX)
    /* Slice 7.6d.N.final.b — interactive busybox-as-/init runner.
     * Same `idle-task` context as the ktest path; the init kthread
     * inherits scheduling cycles via timer preemption. */
    nx_init_busybox_main();
#else
    kprintf("[boot] idle: waiting for work.\n\n");

    for (;;)
        asm volatile("wfi");
#endif
}
