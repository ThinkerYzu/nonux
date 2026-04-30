/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/mm.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_INTERFACE_MM_MSG_H
#define NONUX_INTERFACE_MM_MSG_H

#include <stddef.h>
#include <stdint.h>

/* Op-IDs for `mm` interface.  Stable across versions; removed ops
 * leave their slot as a gravestone — never reuse a freed id. */
enum nx_mm_op_id {
    NX_MM_OP_ALLOC_PAGES = 1,
    NX_MM_OP_FREE_PAGES = 2,
    NX_MM_OP_PAGE_SIZE = 3,
    NX_MM_OP_MAX_ORDER = 4,
};

/* Request: nx_mm_alloc_pages() — Allocate 2^order contiguous pages.  Returns a pointer aligned to */
struct nx_mm_msg_alloc_pages {
    uint32_t order;
};

struct nx_mm_reply_alloc_pages {
    uint64_t rc; /* pointer encoded as u64 */
};

/* Request: nx_mm_free_pages() — Release a block previously returned by `alloc_pages`.  `order` */
struct nx_mm_msg_free_pages {
    uint64_t ptr;
    uint32_t order;
};

struct nx_mm_reply_free_pages {
    int rc; /* always NX_OK; placeholder for void ops */
};

/* Request: nx_mm_page_size() — Page size in bytes.  For the kernel's MMU this is 4096 (matches */
struct nx_mm_msg_page_size {
    char _nx_no_payload;
};

struct nx_mm_reply_page_size {
    size_t rc;
};

/* Request: nx_mm_max_order() — Largest supported order (inclusive).  `alloc_pages` with an */
struct nx_mm_msg_max_order {
    char _nx_no_payload;
};

struct nx_mm_reply_max_order {
    uint32_t rc;
};

#endif /* NONUX_INTERFACE_MM_MSG_H */
