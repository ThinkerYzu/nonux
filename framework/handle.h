#ifndef NX_FRAMEWORK_HANDLE_H
#define NX_FRAMEWORK_HANDLE_H

#include "framework/registry.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Handle framework — Phase 5 slice 5.3.
 *
 * Handles are the native userspace API for every kernel object: channels,
 * memory objects, processes, IRQ sources, files.  Each task (later: each
 * process) owns an `nx_handle_table` that maps small opaque IDs onto
 * (type, rights, object*) triples.  Syscalls name objects by handle ID,
 * the syscall entry looks the ID up in the caller's table, checks the
 * requested operation against the recorded rights, and only then acts on
 * the kernel-side object pointer.
 *
 * Slice 5.3 scope: the table, the type/rights scaffolding, and the four
 * core ops (alloc / lookup / close / duplicate).  No object types wire up
 * here — `HANDLE_CHANNEL` and friends are just enum placeholders until
 * slices 5.6 (channels) and later give them real backing.
 *
 * Handle value layout (32 bits):
 *
 *     +------------ 24 bits ------------+--- 8 bits ---+
 *     | generation (bumped on close)    | index + 1    |
 *     +---------------------------------+--------------+
 *
 *   - `index + 1` so encoded value 0 is reserved (NX_HANDLE_INVALID).
 *   - NX_HANDLE_TABLE_CAPACITY ≤ 255 fits in 8 bits; v1 uses 64.
 *   - A close bumps the slot's generation so a stale handle value that
 *     was captured before close fails lookup (NX_ENOENT) even if the
 *     slot is later reused by a fresh alloc.  Generations are per-slot,
 *     not per-table, so reuse of one slot doesn't invalidate unrelated
 *     handles.
 *
 * Rights attenuation: `nx_handle_duplicate` accepts a rights mask that
 * must be a strict subset of the source handle's rights.  Escalation
 * attempts return NX_EPERM.  This is the only way userspace can narrow
 * a handle before passing it across a channel — there is no path to
 * broaden rights short of a fresh alloc by kernel-side code that owns
 * the object.
 */

/* ---------- Handle types ----------------------------------------------- */

enum nx_handle_type {
    NX_HANDLE_INVALID  = 0,   /* free slot / uninitialised value */
    NX_HANDLE_CHANNEL,        /* IPC channel endpoint (slice 5.6) */
    NX_HANDLE_VMO,            /* virtual memory object */
    NX_HANDLE_PROCESS,        /* process (slice 5.5) */
    NX_HANDLE_THREAD,         /* thread */
    NX_HANDLE_IRQ,            /* interrupt source */
    NX_HANDLE_FILE,           /* file via VFS (Phase 6) */

    NX_HANDLE_TYPE_COUNT,     /* sentinel — keep last */
};

/* ---------- Rights mask ----------------------------------------------- */
/*
 * A bitmask of operations the caller is permitted to perform on the
 * handle's object.  Op-specific rights are checked by the op's syscall
 * entry point before dispatching to the object's handler.  The kernel
 * never grants a handle with rights beyond what the object's type
 * inherently supports; see DESIGN.md §Handle Types for the default set
 * per type.
 */
#define NX_RIGHT_READ       (1U << 0)
#define NX_RIGHT_WRITE      (1U << 1)
#define NX_RIGHT_TRANSFER   (1U << 2)   /* may be passed across a channel */
#define NX_RIGHT_MAP        (1U << 3)   /* VMO: may be mapped into an AS */
#define NX_RIGHT_WAIT       (1U << 4)   /* may wait on signal / IRQ */
#define NX_RIGHT_SIGNAL     (1U << 5)   /* may raise a signal on object */
#define NX_RIGHT_ACK        (1U << 6)   /* IRQ: may acknowledge */
#define NX_RIGHT_SEEK       (1U << 7)   /* FILE: may seek */
#define NX_RIGHT_INFO       (1U << 8)   /* PROCESS/THREAD: may read info */

/* Convenience: all rights set.  Used by in-kernel allocators that want
 * to hand the initial owner every operation on a freshly-created object. */
#define NX_RIGHTS_ALL       0xFFFFFFFFu

/* ---------- Handle value type ---------------------------------------- */

typedef uint32_t nx_handle_t;

#define NX_HANDLE_INVALID        ((nx_handle_t)0)
#define NX_HANDLE_TABLE_CAPACITY 64u   /* v1 per-task cap */

/* ---------- Table ----------------------------------------------------- */

struct nx_handle_entry {
    enum nx_handle_type type;
    uint32_t            rights;
    void               *object;
    uint32_t            generation;   /* bumped on close */
};

struct nx_handle_table {
    struct nx_handle_entry entries[NX_HANDLE_TABLE_CAPACITY];
    size_t                 count;     /* # currently-allocated handles */
};

/* Initialize an empty table.  Must be called before first use; safe to
 * call again to wipe all handles (test fixtures use this for per-case
 * isolation). */
void nx_handle_table_init(struct nx_handle_table *t);

/*
 * Allocate a handle referencing `object` with `type` + `rights`.
 *
 * Returns:
 *   NX_OK      — *out is set to the new handle.
 *   NX_EINVAL  — NULL table / NULL out / invalid type (INVALID or >= COUNT)
 *                / NULL object.
 *   NX_ENOMEM  — table full.
 */
int nx_handle_alloc(struct nx_handle_table *t,
                    enum nx_handle_type     type,
                    uint32_t                rights,
                    void                   *object,
                    nx_handle_t            *out);

/*
 * Look up a handle's backing entry.  Populates any of out_type /
 * out_rights / out_object that are non-NULL; callers that only need
 * one field pass NULL for the rest.
 *
 * Returns:
 *   NX_OK      — handle resolved, out params written.
 *   NX_EINVAL  — NULL table / zero handle / index out of range.
 *   NX_ENOENT  — slot empty or generation mismatch (stale handle).
 */
int nx_handle_lookup(const struct nx_handle_table *t,
                     nx_handle_t                   h,
                     enum nx_handle_type          *out_type,
                     uint32_t                     *out_rights,
                     void                        **out_object);

/*
 * Close a handle.  The slot is zeroed and its generation counter
 * incremented so previously-captured handle values referencing the
 * slot fail subsequent lookups.  No-op on NX_HANDLE_INVALID.
 *
 * Returns:
 *   NX_OK      — handle closed.
 *   NX_EINVAL  — NULL table / out-of-range.
 *   NX_ENOENT  — already-closed / stale generation.
 */
int nx_handle_close(struct nx_handle_table *t, nx_handle_t h);

/*
 * Duplicate `src` with a (possibly reduced) rights mask.  `new_rights`
 * must be a subset of the source handle's rights — any bit set in
 * `new_rights` but not in the source's rights returns NX_EPERM.  The
 * duplicate has the same type + object pointer but an independent
 * generation counter (it's a fresh slot).
 *
 * Returns:
 *   NX_OK      — *out holds the new handle.
 *   NX_EINVAL  — NULL args / bad src.
 *   NX_ENOENT  — src invalid or closed.
 *   NX_EPERM   — new_rights requests more rights than src has.
 *   NX_ENOMEM  — table full.
 */
int nx_handle_duplicate(struct nx_handle_table *t,
                        nx_handle_t             src,
                        uint32_t                new_rights,
                        nx_handle_t            *out);

/* Number of currently-allocated handles.  Test helper. */
size_t nx_handle_table_count(const struct nx_handle_table *t);

#endif /* NX_FRAMEWORK_HANDLE_H */
