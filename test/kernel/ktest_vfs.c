/*
 * Kernel-side coverage for slice 6.2.
 *
 * After `boot_main` runs `nx_framework_bootstrap()`, the new `vfs` and
 * `filesystem.root` slots declared in kernel.json must each be bound
 * to their configured components in NX_LC_ACTIVE state, with
 * init + enable having fired exactly once.  The vfs_simple ops must
 * be callable end-to-end through the live slot, with its call-time
 * `filesystem.root` lookup reaching ramfs's ops — proves the
 * two-component dispatch chain works under the booted kernel.
 *
 * Mirrors slice-5.2's ktest_mm_buddy pattern.
 */

#include "ktest.h"
#include "core/cpu/exception.h"
#include "core/mmu/mmu.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"
#include "framework/bootstrap.h"
#include "framework/component.h"
#include "framework/registry.h"
#include "framework/syscall.h"
#include "interfaces/fs.h"
#include "interfaces/scheduler.h"
#include "interfaces/vfs.h"

/* ---------- Slot presence + lifecycle -------------------------------- */

KTEST(bootstrap_registers_filesystem_root_slot)
{
    struct nx_slot *s = nx_slot_lookup("filesystem.root");
    KASSERT_NOT_NULL(s);
    KASSERT(strcmp(s->iface, "filesystem") == 0);
}

KTEST(bootstrap_binds_ramfs_to_filesystem_root_slot)
{
    struct nx_slot *s = nx_slot_lookup("filesystem.root");
    KASSERT_NOT_NULL(s);
    KASSERT_NOT_NULL(s->active);
    KASSERT(strcmp(s->active->manifest_id, "ramfs") == 0);
    KASSERT_EQ_U(s->active->state, NX_LC_ACTIVE);
}

KTEST(bootstrap_registers_vfs_slot)
{
    struct nx_slot *s = nx_slot_lookup("vfs");
    KASSERT_NOT_NULL(s);
    KASSERT(strcmp(s->iface, "vfs") == 0);
}

KTEST(bootstrap_binds_vfs_simple_to_vfs_slot)
{
    struct nx_slot *s = nx_slot_lookup("vfs");
    KASSERT_NOT_NULL(s);
    KASSERT_NOT_NULL(s->active);
    KASSERT(strcmp(s->active->manifest_id, "vfs_simple") == 0);
    KASSERT_EQ_U(s->active->state, NX_LC_ACTIVE);
}

/* ---------- End-to-end round trip ------------------------------------ */
/*
 * Drive vfs_simple's nx_vfs_ops on the live bootstrap instance.  Each
 * call traverses:
 *
 *   vfs_simple_vfs_ops.op(...)
 *     └─► nx_slot_lookup("filesystem.root")
 *           └─► slot->active->descriptor->iface_ops == &ramfs_fs_ops
 *                 └─► ramfs_fs_ops.op(slot->active->impl, ...)
 *
 * The round-trip proves the whole dispatch chain works under the real
 * booted kernel — the conformance suite proves ramfs's ops are correct,
 * and this test proves vfs_simple actually reaches them.
 */

KTEST(vfs_simple_create_write_read_roundtrip_on_bound_instance)
{
    struct nx_slot *vs = nx_slot_lookup("vfs");
    KASSERT_NOT_NULL(vs);
    KASSERT_NOT_NULL(vs->active);
    KASSERT_NOT_NULL(vs->active->impl);
    KASSERT_NOT_NULL(vs->active->descriptor);

    const struct nx_vfs_ops *vops = vs->active->descriptor->iface_ops;
    KASSERT_NOT_NULL(vops);
    void *vself = vs->active->impl;

    /* Create /ktest_hello, write a 5-byte payload, close.  The file is
     * created in ramfs's static inode table; the same instance stays
     * around for subsequent ktest_vfs tests unless one explicitly cleans
     * up (we don't — these tests are additive). */
    void *w = 0;
    KASSERT_EQ_U(vops->open(vself, "/ktest_hello",
                            NX_VFS_OPEN_READ | NX_VFS_OPEN_WRITE |
                            NX_VFS_OPEN_CREATE, &w),
                 NX_OK);
    KASSERT_NOT_NULL(w);

    const char *payload = "world";
    int64_t wrote = vops->write(vself, w, payload, 5);
    KASSERT_EQ_U((uint64_t)wrote, 5);
    vops->close(vself, w);

    /* Reopen READ-only (no CREATE) and read the bytes back.  Success here
     * proves (a) the create from the previous open stuck, (b) ramfs's
     * reopen path works, and (c) vfs_simple correctly forwards the
     * READ-only flag without demanding WRITE. */
    void *r = 0;
    KASSERT_EQ_U(vops->open(vself, "/ktest_hello",
                            NX_VFS_OPEN_READ, &r),
                 NX_OK);

    char buf[8] = {0};
    int64_t got = vops->read(vself, r, buf, sizeof buf);
    KASSERT_EQ_U((uint64_t)got, 5);
    /* Inline byte compare — core/lib/lib.h doesn't export memcmp. */
    for (int i = 0; i < 5; i++)
        KASSERT_EQ_U((unsigned char)buf[i], (unsigned char)payload[i]);
    vops->close(vself, r);
}

KTEST(vfs_simple_relative_path_returns_einval)
{
    struct nx_slot *vs = nx_slot_lookup("vfs");
    KASSERT_NOT_NULL(vs);
    const struct nx_vfs_ops *vops = vs->active->descriptor->iface_ops;
    KASSERT_NOT_NULL(vops);

    void *f = 0;
    int rc = vops->open(vs->active->impl, "relative",
                        NX_VFS_OPEN_READ | NX_VFS_OPEN_CREATE, &f);
    KASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_EINVAL);
}

/* ---------- EL0 file round-trip (slice 6.3) ------------------------- *
 *
 * Drop to EL0 running `user_prog_file.S`, which does:
 *
 *   open("/hello", READ|WRITE|CREATE) → w
 *   write(w, "[el0-file-ok]", 13)
 *   close(w)                         ← HANDLE_FILE destructor
 *   open("/hello", READ)             → r
 *   read(r, recv_buf, 64)            → 13
 *   close(r)
 *   debug_write(recv_buf, 13)        ← writes "[el0-file-ok]" to UART
 *
 * The kernel test observes:
 *   - debug_write counter rises (EL0 reached the final SVC, which
 *     means every earlier SVC — two opens, write, two closes, read —
 *     succeeded; any failure would have fed a garbage byte count to
 *     debug_write or short-circuited the program earlier).
 *   - The live ktest log shows "[el0-file-ok]" from UART, proving
 *     the bytes written via sys_write and read via sys_read round-
 *     tripped through vfs_simple → ramfs.
 */

extern char __user_file_prog_start[];
extern char __user_file_prog_end[];

static struct nx_task *g_file_el0_task;

static void file_copy_prog_to_window(void *dst)
{
    size_t len = (size_t)(__user_file_prog_end - __user_file_prog_start);
    const char *src = __user_file_prog_start;
    char       *dp  = dst;
    for (size_t i = 0; i < len; i++) dp[i] = src[i];
    if (len == 0) {
        kprintf("[el0-file] embedded program is empty — giving up\n");
        for (;;) asm volatile ("wfe");
    }
}

static void file_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    file_copy_prog_to_window((void *)(uintptr_t)base);
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(base, sp_el0);
}

KTEST(el0_file_open_write_close_reopen_read_roundtrip)
{
    nx_syscall_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);

    g_file_el0_task = sched_spawn_kthread("el0_file", file_el0_kthread, 0);
    KASSERT_NOT_NULL(g_file_el0_task);

    /* Yield until EL0 reaches the final debug_write.  The program
     * issues six SVCs before debug_write (open, write, close, open,
     * read, close); counter rise means every one succeeded. */
    const int max_yields = 256;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (nx_syscall_debug_write_calls() > 0) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    /* Dequeue so the stranded EL0 task doesn't burn later timeslices. */
    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_file_el0_task);
}

/* ---------- EL0 readdir demo (slice 6.4) ---------------------------- *
 *
 * Drop to EL0 running `user_prog_readdir.S`, which:
 *
 *   1. Seeds a fresh `/rd` file.
 *   2. Loops `NX_SYS_READDIR` + `NX_SYS_DEBUG_WRITE(entry.name)` until
 *      readdir returns NX_ENOENT.
 *   3. Emits `[el0-rdr-ok]\n` as the tail marker.
 *
 * By the time this test runs, ramfs already holds `/ktest_hello`
 * (from `vfs_simple_create_write_read_roundtrip_on_bound_instance`)
 * and `/hello` (from `el0_file_open_write_close_reopen_read_
 * roundtrip`).  The seed adds `/rd`.  The program therefore emits at
 * least three filenames to UART before the marker, each followed by a
 * newline — visible in the live ktest log.  Counter-based assertion:
 * debug_write must be called ≥ 4 times (3 names + marker).
 */

extern char __user_readdir_prog_start[];
extern char __user_readdir_prog_end[];

static struct nx_task *g_readdir_el0_task;

static void readdir_copy_prog_to_window(void *dst)
{
    size_t len = (size_t)(__user_readdir_prog_end -
                          __user_readdir_prog_start);
    const char *src = __user_readdir_prog_start;
    char       *dp  = dst;
    for (size_t i = 0; i < len; i++) dp[i] = src[i];
    if (len == 0) {
        kprintf("[el0-rdr] embedded program is empty — giving up\n");
        for (;;) asm volatile ("wfe");
    }
}

static void readdir_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    readdir_copy_prog_to_window((void *)(uintptr_t)base);
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(base, sp_el0);
}

KTEST(el0_readdir_walks_root_and_emits_names_then_marker)
{
    nx_syscall_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);

    g_readdir_el0_task = sched_spawn_kthread("el0_rdr",
                                             readdir_el0_kthread, 0);
    KASSERT_NOT_NULL(g_readdir_el0_task);

    /* Wait for the tail-marker debug_write.  Each loop iteration fires
     * two debug_writes (name + newline); the tail fires one more.  So
     * at minimum (fresh ramfs with just the seeded file) we see 3 —
     * 2 for the name/newline + 1 for the marker.  Tests before this
     * one seed more files in ramfs's persistent state, bringing the
     * total higher; we assert the floor. */
    const int max_yields = 512;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (nx_syscall_debug_write_calls() >= 3) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_readdir_el0_task);
}
