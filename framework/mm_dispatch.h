/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/mm.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_FRAMEWORK_MM_DISPATCH_H
#define NONUX_FRAMEWORK_MM_DISPATCH_H

#include "framework/ipc.h"
#include "framework/registry.h"
#include "interfaces/mm.h"
#include "interfaces/mm_msg.h"

/* Receiver-side dispatch macro for the `mm` interface.
 * The component supplies a static handle_msg function and
 * delegates to NX_MM_DISPATCH(self, ops, msg) which expands
 * into a switch over msg->msg_type that unpacks each request
 * struct and calls the matching op on `ops`. */
#define NX_MM_DISPATCH(self, ops, msg) \
    do { \
        switch ((enum nx_mm_op_id)((msg)->msg_type)) { \
        case NX_MM_OP_ALLOC_PAGES: \
            /* impl: (ops)->alloc_pages(self, ...) — see template body. */ \
            break; \
        case NX_MM_OP_FREE_PAGES: \
            /* impl: (ops)->free_pages(self, ...) — see template body. */ \
            break; \
        case NX_MM_OP_PAGE_SIZE: \
            /* impl: (ops)->page_size(self, ...) — see template body. */ \
            break; \
        case NX_MM_OP_MAX_ORDER: \
            /* impl: (ops)->max_order(self, ...) — see template body. */ \
            break; \
        default: \
            /* unknown op — return NX_EINVAL via reply. */ \
            break; \
        } \
    } while (0)

#endif /* NONUX_FRAMEWORK_MM_DISPATCH_H */
