#include "framework/syscall.h"
#include "framework/channel.h"
#include "framework/handle.h"
#include "framework/registry.h"

#include <stddef.h>
#include <stdint.h>

#if !__STDC_HOSTED__
#include "core/lib/lib.h"              /* uart_putc, memcpy */
#include "core/cpu/exception.h"        /* struct trap_frame */
#include "core/mmu/mmu.h"              /* mmu_user_window_{base,size} */
#else
#include <string.h>                    /* memcpy */
/* Host build: core/cpu/exception.h contains ARM64 inline asm, which
 * won't compile on x86.  Mirror the trap_frame layout locally so host
 * dispatch tests can construct synthetic frames without depending on
 * the CPU header. */
struct trap_frame {
    uint64_t x[31];
    uint64_t sp_el0;
    uint64_t pc;
    uint64_t pstate;
};
#endif

/*
 * Syscall dispatch — slice 5.4.
 *
 * One static dispatch table keyed by syscall number.  Each entry takes
 * the six AArch64-ABI argument registers (x0..x5) as raw uint64_t and
 * returns an nx_status_t; each handler is responsible for type-
 * converting its own args.  A NULL slot is reserved (NX_SYS_RESERVED_0
 * at number 0) so stray `svc #0` with x8 unset gets a crisp NX_EINVAL
 * rather than silently invoking op 0.
 *
 * The table is const, so dispatch is lock-free by construction.  The
 * handlers themselves may touch shared state (the global handle table,
 * the UART) — those touch points manage their own invariants.
 */

typedef nx_status_t (*syscall_fn)(uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t);

/* ---------- Current handle table -------------------------------------- *
 *
 * Slice 5.4 has no per-process concept yet, so the dispatch operates
 * against a single static table.  Slice 5.5 rewrites
 * `nx_syscall_current_table()` to return `&nx_task_current()->
 * handle_table` — every other call site stays unchanged.
 */

static struct nx_handle_table g_kernel_handles;

/* Slice 5.5: test-observable counter of successful debug_write calls.
 * Bumped inside the syscall body on the happy path; tests read via
 * nx_syscall_debug_write_calls() to confirm EL0 code reached the SVC
 * without relying on UART output capture. */
static _Atomic uint64_t g_debug_write_calls;

struct nx_handle_table *nx_syscall_current_table(void)
{
    return &g_kernel_handles;
}

void nx_syscall_reset_for_test(void)
{
    nx_handle_table_init(&g_kernel_handles);
    __atomic_store_n(&g_debug_write_calls, 0, __ATOMIC_RELAXED);
}

uint64_t nx_syscall_debug_write_calls(void)
{
    return __atomic_load_n(&g_debug_write_calls, __ATOMIC_RELAXED);
}

/* ---------- User-pointer copy helpers (slice 5.6) -------------------- *
 *
 * On kernel, bounds-check a candidate user pointer against the MMU's
 * user window — the one EL0-accessible VA range.  If it falls outside,
 * NX_EINVAL; otherwise a plain memcpy.  On host there's no user
 * window, so we trust the pointer and memcpy directly (host tests
 * hand-construct kernel pointers).
 *
 * Proper page-fault-tolerant copy (with an exception-handler fixup)
 * is a later slice; today a bad user pointer that still falls within
 * the window hits memcpy and may scribble on user pages.  That's an
 * acceptable v1 gap: EL0 is trusted research code, and the bounds
 * check catches the "wild kernel-pointer" misuse.
 */
static int copy_from_user(void *kdst, const void *user_src, size_t len)
{
    if (!kdst || !user_src) return NX_EINVAL;
    if (len == 0) return NX_OK;
#if !__STDC_HOSTED__
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t p    = (uint64_t)(uintptr_t)user_src;
    if (p < base || p > base + size)       return NX_EINVAL;
    if (p + len < p || p + len > base + size) return NX_EINVAL;
#endif
    memcpy(kdst, user_src, len);
    return NX_OK;
}

static int copy_to_user(void *user_dst, const void *ksrc, size_t len)
{
    if (!user_dst || !ksrc) return NX_EINVAL;
    if (len == 0) return NX_OK;
#if !__STDC_HOSTED__
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t p    = (uint64_t)(uintptr_t)user_dst;
    if (p < base || p > base + size)       return NX_EINVAL;
    if (p + len < p || p + len > base + size) return NX_EINVAL;
#endif
    memcpy(user_dst, ksrc, len);
    return NX_OK;
}

/* ---------- Syscall bodies ------------------------------------------- */

/*
 * NX_SYS_DEBUG_WRITE — (const char *buf, size_t len).
 *
 * Writes `len` bytes from `buf` to the kernel UART.  Returns the byte
 * count on success (always == len in v1; partial writes arrive when
 * flow control is added).  NX_EINVAL on NULL buf, NX_OK-but-zero on
 * zero len.
 *
 * On host there is no UART — the call fails cleanly with NX_EINVAL so
 * the slice 5.4 plumbing can still link under the host test harness
 * (a host test could mock it if needed; kernel tests exercise the real
 * path).
 */
static nx_status_t sys_debug_write(uint64_t a0, uint64_t a1,
                                   uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *buf = (const char *)(uintptr_t)a0;
    size_t      len = (size_t)a1;
    if (!buf && len > 0) return NX_EINVAL;

#if !__STDC_HOSTED__
    for (size_t i = 0; i < len; i++)
        uart_putc(buf[i]);
#endif
    /* Slice 5.5: bump the test-observable counter on the happy path
     * only.  Atomic because EL0 and kernel-test tasks can both call
     * debug_write under preemption; no correctness rides on this
     * beyond "counter monotonically increases once per successful
     * call", which __ATOMIC_RELAXED delivers. */
    __atomic_fetch_add(&g_debug_write_calls, 1, __ATOMIC_RELAXED);
    return (nx_status_t)len;
}

/*
 * NX_SYS_HANDLE_CLOSE — (nx_handle_t h).
 *
 * Closes a handle in the caller's table, running the object-side
 * destructor first if the type has one.  Slice 5.6 adds the
 * HANDLE_CHANNEL destructor (`nx_channel_endpoint_close`); future
 * handle types (VMO, PROCESS, ...) hook in here too.
 */
static nx_status_t sys_handle_close(uint64_t a0, uint64_t a1,
                                    uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    nx_handle_t h = (nx_handle_t)a0;
    struct nx_handle_table *t = nx_syscall_current_table();

    /* Type-aware close: look up first so we can drop the object's
     * own refcount before the slot itself is freed.  If lookup fails
     * (stale handle), pass straight to nx_handle_close which returns
     * the canonical NX_ENOENT. */
    enum nx_handle_type type;
    void               *object = 0;
    int rc = nx_handle_lookup(t, h, &type, 0, &object);
    if (rc == NX_OK && type == NX_HANDLE_CHANNEL && object) {
        nx_channel_endpoint_close(object);
    }
    return nx_handle_close(t, h);
}

/* ---------- Channel syscalls (slice 5.6) ----------------------------- *
 *
 * All three look identifiers up in the caller's table and dispatch
 * through `framework/channel.c`.  The handles carry the full
 * READ|WRITE|TRANSFER rights at create time — rights attenuation via
 * `nx_handle_duplicate` is how a caller gives out a narrowed handle
 * (e.g. read-only side of a channel).
 */

static nx_status_t sys_channel_create(uint64_t a0, uint64_t a1,
                                      uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    nx_handle_t *user_end0 = (nx_handle_t *)(uintptr_t)a0;
    nx_handle_t *user_end1 = (nx_handle_t *)(uintptr_t)a1;
    if (!user_end0 || !user_end1) return NX_EINVAL;

    struct nx_channel_endpoint *e0 = 0, *e1 = 0;
    int rc = nx_channel_create(&e0, &e1);
    if (rc != NX_OK) return rc;

    struct nx_handle_table *t = nx_syscall_current_table();
    nx_handle_t h0 = NX_HANDLE_INVALID, h1 = NX_HANDLE_INVALID;

    uint32_t rights = NX_RIGHT_READ | NX_RIGHT_WRITE | NX_RIGHT_TRANSFER;
    rc = nx_handle_alloc(t, NX_HANDLE_CHANNEL, rights, e0, &h0);
    if (rc != NX_OK) { nx_channel_endpoint_close(e0);
                        nx_channel_endpoint_close(e1); return rc; }
    rc = nx_handle_alloc(t, NX_HANDLE_CHANNEL, rights, e1, &h1);
    if (rc != NX_OK) {
        /* First alloc succeeded — unwind. */
        nx_handle_close(t, h0);           /* drops the handle slot */
        nx_channel_endpoint_close(e0);
        nx_channel_endpoint_close(e1);
        return rc;
    }

    rc = copy_to_user(user_end0, &h0, sizeof h0);
    if (rc == NX_OK) rc = copy_to_user(user_end1, &h1, sizeof h1);
    if (rc != NX_OK) {
        /* Bad user pointer — close the fresh handles (which also
         * closes the endpoints via sys_handle_close semantics).  We
         * use nx_channel_endpoint_close directly rather than going
         * through the syscall so this is pure framework code. */
        nx_handle_close(t, h0);
        nx_handle_close(t, h1);
        nx_channel_endpoint_close(e0);
        nx_channel_endpoint_close(e1);
        return rc;
    }
    return NX_OK;
}

static int lookup_channel_endpoint(nx_handle_t h, uint32_t need_rights,
                                   struct nx_channel_endpoint **out)
{
    struct nx_handle_table *t = nx_syscall_current_table();
    enum nx_handle_type type;
    uint32_t             rights;
    void                *obj;
    int rc = nx_handle_lookup(t, h, &type, &rights, &obj);
    if (rc != NX_OK) return rc;
    if (type != NX_HANDLE_CHANNEL) return NX_EINVAL;
    if ((rights & need_rights) != need_rights) return NX_EPERM;
    *out = obj;
    return NX_OK;
}

static nx_status_t sys_channel_send(uint64_t a0, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    nx_handle_t  h   = (nx_handle_t)a0;
    const void  *buf = (const void *)(uintptr_t)a1;
    size_t       len = (size_t)a2;
    if (len > NX_CHANNEL_MSG_MAX) return NX_EINVAL;

    struct nx_channel_endpoint *e = 0;
    int rc = lookup_channel_endpoint(h, NX_RIGHT_WRITE, &e);
    if (rc != NX_OK) return rc;

    /* Copy the user payload into a kernel staging buffer before
     * handing it to the channel — prevents a racing user from
     * mutating the bytes after we've checked but before we've
     * enqueued, and decouples the channel layer from user pointers. */
    uint8_t staging[NX_CHANNEL_MSG_MAX];
    rc = copy_from_user(staging, buf, len);
    if (rc != NX_OK) return rc;

    return (nx_status_t)nx_channel_send(e, staging, len);
}

static nx_status_t sys_channel_recv(uint64_t a0, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    nx_handle_t  h   = (nx_handle_t)a0;
    void        *buf = (void *)(uintptr_t)a1;
    size_t       cap = (size_t)a2;
    if (cap == 0) return NX_EINVAL;
    if (cap > NX_CHANNEL_MSG_MAX) cap = NX_CHANNEL_MSG_MAX;

    struct nx_channel_endpoint *e = 0;
    int rc = lookup_channel_endpoint(h, NX_RIGHT_READ, &e);
    if (rc != NX_OK) return rc;

    uint8_t staging[NX_CHANNEL_MSG_MAX];
    int got = nx_channel_recv(e, staging, cap);
    if (got < 0) return got;

    rc = copy_to_user(buf, staging, (size_t)got);
    if (rc != NX_OK) return rc;
    return (nx_status_t)got;
}

/* ---------- Dispatch table ------------------------------------------- */

static const syscall_fn g_syscall_table[NX_SYSCALL_COUNT] = {
    [NX_SYS_RESERVED_0]     = NULL,             /* caught below → NX_EINVAL */
    [NX_SYS_DEBUG_WRITE]    = sys_debug_write,
    [NX_SYS_HANDLE_CLOSE]   = sys_handle_close,
    [NX_SYS_CHANNEL_CREATE] = sys_channel_create,
    [NX_SYS_CHANNEL_SEND]   = sys_channel_send,
    [NX_SYS_CHANNEL_RECV]   = sys_channel_recv,
};

/* ---------- Entry point ---------------------------------------------- */

void nx_syscall_dispatch(struct trap_frame *tf)
{
    if (!tf) return;

    uint64_t num = tf->x[8];
    nx_status_t rc;

    if (num >= NX_SYSCALL_COUNT || g_syscall_table[num] == NULL) {
        rc = NX_EINVAL;
    } else {
        rc = g_syscall_table[num](tf->x[0], tf->x[1], tf->x[2],
                                  tf->x[3], tf->x[4], tf->x[5]);
    }
    tf->x[0] = (uint64_t)rc;
}
