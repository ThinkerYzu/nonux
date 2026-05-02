/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/mm.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_FRAMEWORK_MM_DISPATCH_H
#define NONUX_FRAMEWORK_MM_DISPATCH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "framework/ipc.h"
#include "framework/registry.h"
#include "interfaces/mm.h"
#include "interfaces/mm_msg.h"

/* Receiver-side dispatch function for the `mm` interface.
 * The component's handle_msg delegates to this function, which
 * switches on msg->msg_type, unpacks the per-op request struct,
 * calls the matching op on `ops`, writes the reply struct in-place
 * at msg->payload, and sets msg->reply_payload_len so the
 * dispatcher's build_reply picks up the full per-op output. */
static inline int nx_mm_dispatch(
    void *self, const struct nx_mm_ops *ops,
    struct nx_ipc_message *msg)
{
    switch ((enum nx_mm_op_id)(msg->msg_type)) {
    case NX_MM_OP_ALLOC_PAGES: {
        struct nx_mm_msg_alloc_pages *_req =
            (struct nx_mm_msg_alloc_pages *)msg->payload;
        uint32_t _order = _req->order;
        uint64_t _rcptr = (uint64_t)(uintptr_t)ops->alloc_pages(self, _order);
        struct nx_mm_reply_alloc_pages *_r =
            (struct nx_mm_reply_alloc_pages *)msg->payload;
        _r->rc = _rcptr;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return 0;
    }
    case NX_MM_OP_FREE_PAGES: {
        struct nx_mm_msg_free_pages *_req =
            (struct nx_mm_msg_free_pages *)msg->payload;
        void *_ptr = (void *)(uintptr_t)_req->ptr;
        uint32_t _order = _req->order;
        ops->free_pages(self, _ptr, _order);
        struct nx_mm_reply_free_pages *_r =
            (struct nx_mm_reply_free_pages *)msg->payload;
        _r->rc = 0;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return 0;
    }
    case NX_MM_OP_PAGE_SIZE: {
        size_t _rcsz = ops->page_size(self);
        struct nx_mm_reply_page_size *_r =
            (struct nx_mm_reply_page_size *)msg->payload;
        _r->rc = _rcsz;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return 0;
    }
    case NX_MM_OP_MAX_ORDER: {
        uint32_t _rcu32 = ops->max_order(self);
        struct nx_mm_reply_max_order *_r =
            (struct nx_mm_reply_max_order *)msg->payload;
        _r->rc = _rcu32;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return 0;
    }
    default:
        return NX_EINVAL;
    }
}

#endif /* NONUX_FRAMEWORK_MM_DISPATCH_H */
