#include "ktest.h"
#include "framework/handle.h"

/*
 * Kernel-side coverage for slice 5.3.
 *
 * The handle table is independent of the bootstrap composition — it's
 * just a data-structure API.  These tests exercise it in-kernel to
 * verify the implementation links freestanding (no hidden libc deps)
 * and behaves the same as the host build.
 *
 * Slice 5.5 will embed a handle table in struct nx_task / struct
 * nx_process; until then, tests declare a static table directly.
 */

KTEST(handle_alloc_lookup_close_roundtrip_on_kernel)
{
    static struct nx_handle_table t;
    nx_handle_table_init(&t);

    static int dummy;
    nx_handle_t h;
    KASSERT_EQ_U(nx_handle_alloc(&t, NX_HANDLE_CHANNEL,
                                 NX_RIGHT_READ | NX_RIGHT_WRITE,
                                 &dummy, &h), NX_OK);
    KASSERT(h != NX_HANDLE_INVALID);

    enum nx_handle_type type;
    uint32_t rights;
    void *obj;
    KASSERT_EQ_U(nx_handle_lookup(&t, h, &type, &rights, &obj), NX_OK);
    KASSERT_EQ_U((uint64_t)type, (uint64_t)NX_HANDLE_CHANNEL);
    KASSERT_EQ_U(rights, NX_RIGHT_READ | NX_RIGHT_WRITE);
    KASSERT(obj == &dummy);

    KASSERT_EQ_U(nx_handle_close(&t, h), NX_OK);
    KASSERT_EQ_U(nx_handle_lookup(&t, h, NULL, NULL, NULL), NX_ENOENT);
}

KTEST(handle_duplicate_rights_attenuation_on_kernel)
{
    static struct nx_handle_table t;
    nx_handle_table_init(&t);

    static int dummy;
    nx_handle_t src;
    nx_handle_alloc(&t, NX_HANDLE_VMO,
                    NX_RIGHT_READ | NX_RIGHT_WRITE | NX_RIGHT_MAP,
                    &dummy, &src);

    /* Attenuation works: drop write. */
    nx_handle_t readonly;
    KASSERT_EQ_U(nx_handle_duplicate(&t, src,
                                     NX_RIGHT_READ | NX_RIGHT_MAP,
                                     &readonly), NX_OK);
    uint32_t rights;
    KASSERT_EQ_U(nx_handle_lookup(&t, readonly, NULL, &rights, NULL), NX_OK);
    KASSERT_EQ_U(rights, NX_RIGHT_READ | NX_RIGHT_MAP);

    /* Escalation fails: ask for TRANSFER which src doesn't have. */
    nx_handle_t bad;
    KASSERT_EQ_U(nx_handle_duplicate(&t, src,
                                     NX_RIGHT_READ | NX_RIGHT_TRANSFER,
                                     &bad), NX_EPERM);
}

KTEST(handle_stale_value_after_close_does_not_resolve_to_reused_slot)
{
    static struct nx_handle_table t;
    nx_handle_table_init(&t);

    static int a, b;
    nx_handle_t h1, h2;
    nx_handle_alloc(&t, NX_HANDLE_CHANNEL, NX_RIGHT_READ, &a, &h1);
    nx_handle_close(&t, h1);
    nx_handle_alloc(&t, NX_HANDLE_CHANNEL, NX_RIGHT_WRITE, &b, &h2);

    /* h1 and h2 almost certainly alias the same slot (it's the first
     * free one).  Generations differ, so h1 must NOT resolve to b. */
    KASSERT_EQ_U(nx_handle_lookup(&t, h1, NULL, NULL, NULL), NX_ENOENT);

    /* h2 resolves to b cleanly. */
    void *obj;
    KASSERT_EQ_U(nx_handle_lookup(&t, h2, NULL, NULL, &obj), NX_OK);
    KASSERT(obj == &b);
}
