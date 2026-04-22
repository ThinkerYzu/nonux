#include "framework/handle.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Handle framework implementation — slice 5.3.
 *
 * Encoding recap:
 *
 *     handle = (generation << 8) | (index + 1)
 *
 *   - Value 0 is reserved for NX_HANDLE_INVALID.
 *   - Low 8 bits hold `index + 1`; decode by subtracting 1.
 *   - High 24 bits hold the slot's generation.  `nx_handle_close` bumps
 *     the slot's in-table generation counter so any previously-captured
 *     handle value with the old generation stops resolving.
 *
 * No free-list: with NX_HANDLE_TABLE_CAPACITY = 64 a linear scan for an
 * empty slot is one cache line and trivially correct.  A fanout to a
 * free-list (or hashed table) lands if v2 bumps the cap to four digits.
 *
 * Concurrency: v1 is single-CPU per handle-table owner (each task/process
 * owns one table).  No locking inside the table.  SMP will wrap at the
 * syscall entry path, not here.
 */

#define INDEX_BITS   8u
#define INDEX_MASK   ((1u << INDEX_BITS) - 1u)
#define GEN_SHIFT    INDEX_BITS

static inline nx_handle_t encode_handle(uint32_t generation, size_t index)
{
    /* index + 1 keeps encoded value 0 reserved for NX_HANDLE_INVALID.
     * With 8 index bits and a 64-entry table, encoded indices 1..64
     * fit comfortably. */
    return (generation << GEN_SHIFT) | (nx_handle_t)(index + 1);
}

static inline int decode_index(nx_handle_t h, size_t *out_index)
{
    if (h == NX_HANDLE_INVALID) return 0;
    uint32_t enc = h & INDEX_MASK;
    if (enc == 0 || enc > NX_HANDLE_TABLE_CAPACITY) return 0;
    *out_index = (size_t)(enc - 1);
    return 1;
}

static inline uint32_t decode_generation(nx_handle_t h)
{
    return h >> GEN_SHIFT;
}

/* ---------- Table ------------------------------------------------------ */

void nx_handle_table_init(struct nx_handle_table *t)
{
    if (!t) return;
    for (size_t i = 0; i < NX_HANDLE_TABLE_CAPACITY; i++) {
        t->entries[i].type       = NX_HANDLE_INVALID;
        t->entries[i].rights     = 0;
        t->entries[i].object     = NULL;
        /* Generation deliberately preserved across `init` so that a
         * table-init on a previously-used table still invalidates any
         * lingering handle values from the prior incarnation.  Tests
         * that need a deterministic starting generation can zero the
         * struct memory themselves before calling init. */
        /* NOTE: init() called on a zero-initialised struct (as produced
         * by calloc or static storage) leaves generation at 0 — the
         * common case for fresh tables.  No special-casing needed. */
    }
    t->count = 0;
}

size_t nx_handle_table_count(const struct nx_handle_table *t)
{
    return t ? t->count : 0;
}

/* ---------- Alloc ------------------------------------------------------ */

int nx_handle_alloc(struct nx_handle_table *t,
                    enum nx_handle_type     type,
                    uint32_t                rights,
                    void                   *object,
                    nx_handle_t            *out)
{
    if (!t || !out) return NX_EINVAL;
    if (type == NX_HANDLE_INVALID || type >= NX_HANDLE_TYPE_COUNT)
        return NX_EINVAL;
    if (!object) return NX_EINVAL;

    for (size_t i = 0; i < NX_HANDLE_TABLE_CAPACITY; i++) {
        struct nx_handle_entry *e = &t->entries[i];
        if (e->type != NX_HANDLE_INVALID) continue;
        e->type   = type;
        e->rights = rights;
        e->object = object;
        /* Generation stays as-is — close incremented it when the slot
         * was freed, and alloc inherits that same value so the newly-
         * returned handle encodes it.  Bumping here instead would leak
         * one generation value per alloc/free pair. */
        t->count++;
        *out = encode_handle(e->generation, i);
        return NX_OK;
    }
    return NX_ENOMEM;
}

/* ---------- Lookup ---------------------------------------------------- */

int nx_handle_lookup(const struct nx_handle_table *t,
                     nx_handle_t                   h,
                     enum nx_handle_type          *out_type,
                     uint32_t                     *out_rights,
                     void                        **out_object)
{
    if (!t) return NX_EINVAL;
    size_t idx;
    if (!decode_index(h, &idx)) return NX_EINVAL;

    const struct nx_handle_entry *e = &t->entries[idx];
    if (e->type == NX_HANDLE_INVALID) return NX_ENOENT;
    if (e->generation != decode_generation(h)) return NX_ENOENT;

    if (out_type)   *out_type   = e->type;
    if (out_rights) *out_rights = e->rights;
    if (out_object) *out_object = e->object;
    return NX_OK;
}

/* ---------- Close ----------------------------------------------------- */

int nx_handle_close(struct nx_handle_table *t, nx_handle_t h)
{
    if (!t) return NX_EINVAL;
    size_t idx;
    if (!decode_index(h, &idx)) return NX_EINVAL;

    struct nx_handle_entry *e = &t->entries[idx];
    if (e->type == NX_HANDLE_INVALID) return NX_ENOENT;
    if (e->generation != decode_generation(h)) return NX_ENOENT;

    e->type   = NX_HANDLE_INVALID;
    e->rights = 0;
    e->object = NULL;
    /* Bump generation LAST so a racing lookup either sees the fully-
     * zeroed entry (and returns NX_ENOENT on type check) or the old
     * generation (and returns NX_ENOENT on generation check).  Either
     * way, callers never observe a half-closed slot. */
    e->generation++;
    t->count--;
    return NX_OK;
}

/* ---------- Duplicate ------------------------------------------------- */

int nx_handle_duplicate(struct nx_handle_table *t,
                        nx_handle_t             src,
                        uint32_t                new_rights,
                        nx_handle_t            *out)
{
    if (!t || !out) return NX_EINVAL;

    enum nx_handle_type src_type;
    uint32_t            src_rights;
    void               *src_object;
    int rc = nx_handle_lookup(t, src, &src_type, &src_rights, &src_object);
    if (rc != NX_OK) return rc;

    /* Attenuation-only: every bit set in new_rights must also be set
     * in src_rights.  Any bit in new_rights but not src_rights is an
     * escalation attempt → NX_EPERM. */
    if ((new_rights & ~src_rights) != 0) return NX_EPERM;

    return nx_handle_alloc(t, src_type, new_rights, src_object, out);
}
