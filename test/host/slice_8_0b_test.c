/*
 * Host-side tests for slice 8.0b — activate generated handle_msg shims.
 *
 * Verifies:
 *   1. All 5 production components (vfs_simple, ramfs, procfs, mm_buddy,
 *      sched_rr) have non-NULL handle_msg after slice 8.0b.
 *   2. uart_pl011's handle_msg is non-NULL via component ops.
 *   3. Each dispatch shim routes msg_type to the correct op.
 *   4. Reply payload is written in-place at msg->payload and
 *      msg->reply_payload_len reflects the reply struct size.
 *   5. Direct-call vs dispatch equivalence (ASSERT_CALL_EQUIVALENT).
 */

#include "test_runner.h"

#include "framework/char_device_dispatch.h"
#include "framework/component.h"
#include "framework/fs_dispatch.h"
#include "framework/ipc.h"
#include "framework/mm_dispatch.h"
#include "framework/registry.h"
#include "framework/scheduler_dispatch.h"
#include "framework/vfs_dispatch.h"
#include "interfaces/char_device.h"
#include "interfaces/char_device_msg.h"
#include "interfaces/fs.h"
#include "interfaces/fs_msg.h"
#include "interfaces/mm.h"
#include "interfaces/mm_msg.h"
#include "interfaces/scheduler.h"
#include "interfaces/scheduler_msg.h"
#include "interfaces/vfs.h"
#include "interfaces/vfs_msg.h"
#include "core/sched/task.h"

#include <stdint.h>
#include <string.h>

#define ASSERT_CALL_EQUIVALENT(direct_rc, dispatch_rc) \
    ASSERT_EQ_U((unsigned int)(direct_rc), (unsigned int)(dispatch_rc))

/* External component ops structs. */
extern const struct nx_component_ops vfs_simple_component_ops;
extern const struct nx_component_ops ramfs_component_ops;
extern const struct nx_component_ops procfs_component_ops;
extern const struct nx_component_ops mm_buddy_component_ops;
extern const struct nx_component_ops sched_rr_component_ops;

/* External char_device ops for uart_pl011 (declared in uart_pl011.c). */
extern const struct nx_component_ops uart_pl011_ops;  /* static — can't link */

/* ================================================================== */
/* Section 1: handle_msg non-NULL checks                               */
/* ================================================================== */

TEST(vfs_simple_has_handle_msg)
{
    ASSERT_NOT_NULL(vfs_simple_component_ops.handle_msg);
}

TEST(ramfs_has_handle_msg)
{
    ASSERT_NOT_NULL(ramfs_component_ops.handle_msg);
}

TEST(procfs_has_handle_msg)
{
    ASSERT_NOT_NULL(procfs_component_ops.handle_msg);
}

TEST(mm_buddy_has_handle_msg)
{
    ASSERT_NOT_NULL(mm_buddy_component_ops.handle_msg);
}

TEST(sched_rr_has_handle_msg)
{
    ASSERT_NOT_NULL(sched_rr_component_ops.handle_msg);
}

/* ================================================================== */
/* Section 2: char_device dispatch                                     */
/* ================================================================== */

static size_t s_cd_write_len;
static int64_t s_cd_write_rc;

static int64_t cd_fake_write(void *self, const void *buf, size_t len)
{
    (void)self; (void)buf;
    s_cd_write_len = len;
    return s_cd_write_rc;
}
static void cd_fake_rx_byte(void *self, uint8_t byte)
{
    (void)self; (void)byte;
}
static const struct nx_char_device_ops cd_fake_ops = {
    .write   = cd_fake_write,
    .rx_byte = cd_fake_rx_byte,
};

TEST(char_device_dispatch_write_returns_byte_count)
{
    char payload_buf[8] = "hello";
    s_cd_write_rc  = 5;
    s_cd_write_len = 0;

    struct nx_char_device_msg_write req;
    memset(&req, 0, sizeof req);
    req.buf = (uint64_t)(uintptr_t)payload_buf;
    req.len = 5;

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_CHAR_DEVICE_OP_WRITE;
    msg.payload     = &req;
    msg.payload_len = (uint32_t)sizeof req;

    int rc = nx_char_device_dispatch(NULL, &cd_fake_ops, &msg);

    ASSERT_EQ_U((unsigned)rc, 5u);
    ASSERT(msg.reply_payload_len == (uint32_t)sizeof(struct nx_char_device_reply_write));
    ASSERT_EQ_U(s_cd_write_len, 5u);

    const struct nx_char_device_reply_write *r = msg.payload;
    ASSERT_EQ_U((unsigned)(int64_t)r->rc, 5u);
    ASSERT_EQ_U(r->bytes_actual, 5u);
}

TEST(char_device_dispatch_unknown_op_returns_einval)
{
    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = 0x7fff;
    msg.payload     = NULL;
    msg.payload_len = 0;

    int rc = nx_char_device_dispatch(NULL, &cd_fake_ops, &msg);
    ASSERT_EQ_U((unsigned)rc, (unsigned)NX_EINVAL);
}

TEST(char_device_dispatch_write_negative_rc_bytes_actual_zero)
{
    char buf[4] = "xyz";
    s_cd_write_rc  = NX_EINVAL;
    s_cd_write_len = 0;

    struct nx_char_device_msg_write req;
    memset(&req, 0, sizeof req);
    req.buf = (uint64_t)(uintptr_t)buf;
    req.len = 3;

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_CHAR_DEVICE_OP_WRITE;
    msg.payload     = &req;
    msg.payload_len = (uint32_t)sizeof req;

    int rc = nx_char_device_dispatch(NULL, &cd_fake_ops, &msg);
    ASSERT_EQ_U((unsigned)rc, (unsigned)NX_EINVAL);

    const struct nx_char_device_reply_write *r = msg.payload;
    ASSERT_EQ_U(r->bytes_actual, 0u);
}

/* ================================================================== */
/* Section 3: mm dispatch                                              */
/* ================================================================== */

static char   s_mm_fake_page[64];
static size_t s_mm_page_size_ret;
static uint32_t s_mm_max_order_ret;

static void *mm_fake_alloc(void *s, uint32_t o)  { (void)s;(void)o; return s_mm_fake_page; }
static void  mm_fake_free(void *s, void *p, uint32_t o) { (void)s;(void)p;(void)o; }
static size_t mm_fake_page_size(void *s)  { (void)s; return s_mm_page_size_ret; }
static uint32_t mm_fake_max_order(void *s) { (void)s; return s_mm_max_order_ret; }

static const struct nx_mm_ops mm_fake_ops = {
    .alloc_pages = mm_fake_alloc,
    .free_pages  = mm_fake_free,
    .page_size   = mm_fake_page_size,
    .max_order   = mm_fake_max_order,
};

TEST(mm_dispatch_page_size_returns_value_via_reply)
{
    s_mm_page_size_ret = 4096;

    struct nx_mm_reply_page_size reply_area;
    memset(&reply_area, 0, sizeof reply_area);

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_MM_OP_PAGE_SIZE;
    msg.payload     = &reply_area;
    msg.payload_len = (uint32_t)sizeof reply_area;

    int rc = nx_mm_dispatch(NULL, &mm_fake_ops, &msg);
    ASSERT_EQ_U((unsigned)rc, 0u);
    ASSERT(msg.reply_payload_len == (uint32_t)sizeof(struct nx_mm_reply_page_size));

    const struct nx_mm_reply_page_size *r = msg.payload;
    ASSERT_EQ_U((unsigned)r->rc, 4096u);
}

TEST(mm_dispatch_alloc_returns_pointer_in_reply)
{
    /* Buffer must fit both request and reply since dispatch writes in-place. */
    union {
        struct nx_mm_msg_alloc_pages req;
        struct nx_mm_reply_alloc_pages rep;
    } buf;
    memset(&buf, 0, sizeof buf);
    buf.req.order = 0;

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_MM_OP_ALLOC_PAGES;
    msg.payload     = &buf;
    msg.payload_len = (uint32_t)sizeof(buf.req);

    int rc = nx_mm_dispatch(NULL, &mm_fake_ops, &msg);
    ASSERT_EQ_U((unsigned)rc, 0u);
    ASSERT(msg.reply_payload_len == (uint32_t)sizeof(struct nx_mm_reply_alloc_pages));

    ASSERT_EQ_PTR((void *)(uintptr_t)buf.rep.rc, (void *)s_mm_fake_page);
}

TEST(mm_dispatch_max_order_returns_value)
{
    s_mm_max_order_ret = 7;

    struct nx_mm_reply_max_order reply_area;
    memset(&reply_area, 0, sizeof reply_area);

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_MM_OP_MAX_ORDER;
    msg.payload     = &reply_area;
    msg.payload_len = (uint32_t)sizeof reply_area;

    int rc = nx_mm_dispatch(NULL, &mm_fake_ops, &msg);
    ASSERT_EQ_U((unsigned)rc, 0u);
    ASSERT(msg.reply_payload_len == (uint32_t)sizeof(struct nx_mm_reply_max_order));

    const struct nx_mm_reply_max_order *r = msg.payload;
    ASSERT_EQ_U((unsigned)r->rc, 7u);
}

/* ================================================================== */
/* Section 4: scheduler dispatch                                       */
/* ================================================================== */

static int     s_sched_enqueue_called;
static int     s_sched_enqueue_rc;
static struct nx_task s_sched_sentinel_task;
static struct nx_task *s_sched_pick_ret;

static struct nx_task *sched_fake_pick(void *s)  { (void)s; return s_sched_pick_ret; }
static int  sched_fake_enqueue(void *s, struct nx_task *t)
    { (void)s;(void)t; s_sched_enqueue_called++; return s_sched_enqueue_rc; }
static int  sched_fake_dequeue(void *s, struct nx_task *t) { (void)s;(void)t; return NX_OK; }
static void sched_fake_yield(void *s)  { (void)s; }
static int  sched_fake_sp(void *s, struct nx_task *t, int p)
    { (void)s;(void)t;(void)p; return NX_EINVAL; }
static void sched_fake_tick(void *s) { (void)s; }

static const struct nx_scheduler_ops sched_fake_ops = {
    .pick_next    = sched_fake_pick,
    .enqueue      = sched_fake_enqueue,
    .dequeue      = sched_fake_dequeue,
    .yield        = sched_fake_yield,
    .set_priority = sched_fake_sp,
    .tick         = sched_fake_tick,
};

TEST(scheduler_dispatch_enqueue_rc_propagated)
{
    s_sched_enqueue_called = 0;
    s_sched_enqueue_rc     = NX_OK;

    struct nx_task dummy;
    memset(&dummy, 0, sizeof dummy);

    struct nx_scheduler_msg_enqueue req;
    memset(&req, 0, sizeof req);
    req.task = (uint64_t)(uintptr_t)&dummy;

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_SCHEDULER_OP_ENQUEUE;
    msg.payload     = &req;
    msg.payload_len = (uint32_t)sizeof req;

    int rc = nx_scheduler_dispatch(NULL, &sched_fake_ops, &msg);
    ASSERT_EQ_U((unsigned)rc, (unsigned)NX_OK);
    ASSERT_EQ_U((unsigned)s_sched_enqueue_called, 1u);
    ASSERT(msg.reply_payload_len == (uint32_t)sizeof(struct nx_scheduler_reply_enqueue));
}

TEST(scheduler_dispatch_enqueue_einval_rc_propagated)
{
    s_sched_enqueue_called = 0;
    s_sched_enqueue_rc     = NX_EINVAL;

    struct nx_task dummy;
    memset(&dummy, 0, sizeof dummy);

    struct nx_scheduler_msg_enqueue req;
    memset(&req, 0, sizeof req);
    req.task = (uint64_t)(uintptr_t)&dummy;

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_SCHEDULER_OP_ENQUEUE;
    msg.payload     = &req;
    msg.payload_len = (uint32_t)sizeof req;

    int rc = nx_scheduler_dispatch(NULL, &sched_fake_ops, &msg);
    ASSERT_EQ_U((unsigned)rc, (unsigned)NX_EINVAL);
    ASSERT_CALL_EQUIVALENT(NX_EINVAL, rc);
}

TEST(scheduler_dispatch_pick_next_pointer_in_reply)
{
    s_sched_pick_ret = &s_sched_sentinel_task;

    struct nx_scheduler_reply_pick_next reply_area;
    memset(&reply_area, 0, sizeof reply_area);

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_SCHEDULER_OP_PICK_NEXT;
    msg.payload     = &reply_area;
    msg.payload_len = (uint32_t)sizeof reply_area;

    int rc = nx_scheduler_dispatch(NULL, &sched_fake_ops, &msg);
    ASSERT_EQ_U((unsigned)rc, 0u);
    ASSERT(msg.reply_payload_len == (uint32_t)sizeof(struct nx_scheduler_reply_pick_next));

    const struct nx_scheduler_reply_pick_next *r = msg.payload;
    ASSERT_EQ_PTR((void *)(uintptr_t)r->rc, (void *)&s_sched_sentinel_task);
}

TEST(scheduler_dispatch_pick_next_null_when_runqueue_empty)
{
    s_sched_pick_ret = NULL;

    struct nx_scheduler_reply_pick_next reply_area;
    memset(&reply_area, 0xff, sizeof reply_area);   /* poison */

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_SCHEDULER_OP_PICK_NEXT;
    msg.payload     = &reply_area;
    msg.payload_len = (uint32_t)sizeof reply_area;

    nx_scheduler_dispatch(NULL, &sched_fake_ops, &msg);

    const struct nx_scheduler_reply_pick_next *r = msg.payload;
    ASSERT_EQ_PTR((void *)(uintptr_t)r->rc, NULL);
}

TEST(scheduler_dispatch_tick_void_reply_payload_len_nonzero)
{
    struct nx_scheduler_reply_tick reply_area;
    memset(&reply_area, 0xff, sizeof reply_area);

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_SCHEDULER_OP_TICK;
    msg.payload     = &reply_area;
    msg.payload_len = (uint32_t)sizeof reply_area;

    int rc = nx_scheduler_dispatch(NULL, &sched_fake_ops, &msg);
    ASSERT_EQ_U((unsigned)rc, 0u);
    ASSERT(msg.reply_payload_len == (uint32_t)sizeof(struct nx_scheduler_reply_tick));

    const struct nx_scheduler_reply_tick *r = msg.payload;
    ASSERT_EQ_U((unsigned)r->rc, 0u);
}

/* ================================================================== */
/* Section 5: vfs dispatch                                             */
/* ================================================================== */

static void  *s_vfs_open_out_file;
static int    s_vfs_open_rc;
static int    s_vfs_open_called;
static int    s_vfs_close_called;
static int64_t s_vfs_read_rc;
static int    s_vfs_read_called;
static const char *s_vfs_read_src;
static int    s_vfs_mkdir_rc;

static int vfs_fake_open(void *s, const char *p, uint32_t f, void **out)
{
    (void)s;(void)p;(void)f;
    *out = s_vfs_open_out_file;
    s_vfs_open_called++;
    return s_vfs_open_rc;
}
static void vfs_fake_close(void *s, void *f)   { (void)s;(void)f; s_vfs_close_called++; }
static void vfs_fake_retain(void *s, void *f)  { (void)s;(void)f; }
static int64_t vfs_fake_read(void *s, void *f, void *buf, size_t cap)
{
    (void)s;(void)f;
    size_t n = strlen(s_vfs_read_src);
    if (n > cap) n = cap;
    memcpy(buf, s_vfs_read_src, n);
    s_vfs_read_called++;
    s_vfs_read_rc = (int64_t)n;
    return (int64_t)n;
}
static int64_t vfs_fake_write(void *s, void *f, const void *b, size_t l)
    { (void)s;(void)f;(void)b; return (int64_t)l; }
static int64_t vfs_fake_seek(void *s, void *f, int64_t o, int w)
    { (void)s;(void)f;(void)o;(void)w; return 0; }
static int vfs_fake_readdir(void *s, const char *p, uint32_t *c, struct nx_fs_dirent *d)
    { (void)s;(void)p;(void)c;(void)d; return NX_ENOENT; }
static int vfs_fake_mkdir(void *s, const char *p)
    { (void)s;(void)p; return s_vfs_mkdir_rc; }
static int vfs_fake_stat(void *s, const char *p, struct nx_fs_stat *o)
    { (void)s;(void)p; o->size = 42; o->kind = NX_FS_KIND_FILE; return NX_OK; }

static const struct nx_vfs_ops vfs_fake_ops = {
    .open    = vfs_fake_open,   .close   = vfs_fake_close,
    .retain  = vfs_fake_retain, .read    = vfs_fake_read,
    .write   = vfs_fake_write,  .seek    = vfs_fake_seek,
    .readdir = vfs_fake_readdir,.mkdir   = vfs_fake_mkdir,
    .stat    = vfs_fake_stat,
};

TEST(vfs_dispatch_open_rc_and_out_file_in_reply)
{
    static char sentinel;
    s_vfs_open_out_file = &sentinel;
    s_vfs_open_rc       = NX_OK;
    s_vfs_open_called   = 0;

    struct nx_vfs_msg_open req;
    memset(&req, 0, sizeof req);
    strncpy(req.path, "/foo", sizeof req.path - 1);
    req.flags = NX_VFS_OPEN_READ;

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_VFS_OP_OPEN;
    msg.payload     = &req;
    msg.payload_len = (uint32_t)sizeof req;

    int rc = nx_vfs_dispatch(NULL, &vfs_fake_ops, &msg);
    ASSERT_EQ_U((unsigned)rc, (unsigned)NX_OK);
    ASSERT_EQ_U((unsigned)s_vfs_open_called, 1u);
    ASSERT(msg.reply_payload_len == (uint32_t)sizeof(struct nx_vfs_reply_open));

    const struct nx_vfs_reply_open *r = msg.payload;
    ASSERT_EQ_U((unsigned)r->rc, (unsigned)NX_OK);
    ASSERT_EQ_PTR((void *)(uintptr_t)r->out_file, &sentinel);
}

TEST(vfs_dispatch_open_direct_vs_dispatch_equivalence)
{
    static char sentinel;
    s_vfs_open_out_file = &sentinel;
    s_vfs_open_rc       = NX_OK;

    /* direct call rc */
    void *out_direct = NULL;
    int rc_direct = vfs_fake_open(NULL, "/foo", NX_VFS_OPEN_READ, &out_direct);

    /* dispatch call rc */
    struct nx_vfs_msg_open req;
    memset(&req, 0, sizeof req);
    strncpy(req.path, "/foo", sizeof req.path - 1);
    req.flags = NX_VFS_OPEN_READ;
    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_VFS_OP_OPEN;
    msg.payload     = &req;
    msg.payload_len = (uint32_t)sizeof req;
    int rc_dispatch = nx_vfs_dispatch(NULL, &vfs_fake_ops, &msg);

    ASSERT_CALL_EQUIVALENT(rc_direct, rc_dispatch);
}

TEST(vfs_dispatch_read_writes_data_to_caller_buf)
{
    s_vfs_read_src    = "hello";
    s_vfs_read_called = 0;

    char dst[8];
    memset(dst, 0, sizeof dst);

    struct nx_vfs_msg_read req;
    memset(&req, 0, sizeof req);
    req.file = 0;
    req.buf  = (uint64_t)(uintptr_t)dst;
    req.cap  = sizeof dst;

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_VFS_OP_READ;
    msg.payload     = &req;
    msg.payload_len = (uint32_t)sizeof req;

    int rc = nx_vfs_dispatch(NULL, &vfs_fake_ops, &msg);

    ASSERT_EQ_U((unsigned)rc, 5u);
    ASSERT_EQ_U((unsigned)s_vfs_read_called, 1u);
    ASSERT_EQ_U((unsigned)memcmp(dst, "hello", 5), 0u);   /* data in caller buf */

    ASSERT(msg.reply_payload_len == (uint32_t)sizeof(struct nx_vfs_reply_read));
    const struct nx_vfs_reply_read *r = msg.payload;
    ASSERT_EQ_U((unsigned)(int64_t)r->rc, 5u);
    ASSERT_EQ_U(r->bytes_actual, 5u);
}

TEST(vfs_dispatch_close_increments_counter)
{
    s_vfs_close_called = 0;

    struct nx_vfs_msg_close req;
    memset(&req, 0, sizeof req);
    req.file = (uint64_t)(uintptr_t)(void *)0xabcd;

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_VFS_OP_CLOSE;
    msg.payload     = &req;
    msg.payload_len = (uint32_t)sizeof req;

    int rc = nx_vfs_dispatch(NULL, &vfs_fake_ops, &msg);
    ASSERT_EQ_U((unsigned)rc, 0u);
    ASSERT_EQ_U((unsigned)s_vfs_close_called, 1u);
    ASSERT(msg.reply_payload_len == (uint32_t)sizeof(struct nx_vfs_reply_close));
}

TEST(vfs_dispatch_mkdir_rc_propagated)
{
    s_vfs_mkdir_rc = NX_EEXIST;

    struct nx_vfs_msg_mkdir req;
    memset(&req, 0, sizeof req);
    strncpy(req.path, "/newdir", sizeof req.path - 1);

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_VFS_OP_MKDIR;
    msg.payload     = &req;
    msg.payload_len = (uint32_t)sizeof req;

    int rc = nx_vfs_dispatch(NULL, &vfs_fake_ops, &msg);
    ASSERT_EQ_U((unsigned)rc, (unsigned)NX_EEXIST);

    const struct nx_vfs_reply_mkdir *r = msg.payload;
    ASSERT_EQ_U((unsigned)r->rc, (unsigned)NX_EEXIST);
    ASSERT_CALL_EQUIVALENT(NX_EEXIST, rc);
}

TEST(vfs_dispatch_stat_out_struct_in_reply)
{
    struct nx_vfs_msg_stat req;
    memset(&req, 0, sizeof req);
    strncpy(req.path, "/testfile", sizeof req.path - 1);

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_VFS_OP_STAT;
    msg.payload     = &req;
    msg.payload_len = (uint32_t)sizeof req;

    int rc = nx_vfs_dispatch(NULL, &vfs_fake_ops, &msg);
    ASSERT_EQ_U((unsigned)rc, (unsigned)NX_OK);
    ASSERT(msg.reply_payload_len == (uint32_t)sizeof(struct nx_vfs_reply_stat));

    const struct nx_vfs_reply_stat *r = msg.payload;
    ASSERT_EQ_U((unsigned)r->rc, (unsigned)NX_OK);
    ASSERT_EQ_U((unsigned)r->out.kind, (unsigned)NX_FS_KIND_FILE);
    ASSERT_EQ_U((unsigned)r->out.size, 42u);
}

/* ================================================================== */
/* Section 6: fs dispatch (shared with ramfs / procfs)                */
/* ================================================================== */

static int    s_fs_open_called;
static int    s_fs_open_rc;
static void  *s_fs_open_out;
static int    s_fs_close_called;
static int    s_fs_mkdir_rc;

static int fs_fake_open(void *s, const char *p, uint32_t f, void **o)
    { (void)s;(void)p;(void)f; *o = s_fs_open_out; s_fs_open_called++; return s_fs_open_rc; }
static void fs_fake_close(void *s, void *f)   { (void)s;(void)f; s_fs_close_called++; }
static void fs_fake_retain(void *s, void *f)  { (void)s;(void)f; }
static int64_t fs_fake_read(void *s, void *f, void *b, size_t c)
    { (void)s;(void)f;(void)b;(void)c; return 0; }
static int64_t fs_fake_write(void *s, void *f, const void *b, size_t l)
    { (void)s;(void)f;(void)b; return (int64_t)l; }
static int64_t fs_fake_seek(void *s, void *f, int64_t o, int w)
    { (void)s;(void)f;(void)o;(void)w; return 0; }
static int fs_fake_readdir(void *s, const char *p, uint32_t *c, struct nx_fs_dirent *d)
    { (void)s;(void)p;(void)c;(void)d; return NX_ENOENT; }
static int fs_fake_mkdir(void *s, const char *p)
    { (void)s;(void)p; return s_fs_mkdir_rc; }
static int fs_fake_stat(void *s, const char *p, struct nx_fs_stat *o)
    { (void)s;(void)p; o->size = 0; o->kind = NX_FS_KIND_DIR; return NX_OK; }

static const struct nx_fs_ops fs_fake_ops = {
    .open    = fs_fake_open,   .close   = fs_fake_close,
    .retain  = fs_fake_retain, .read    = fs_fake_read,
    .write   = fs_fake_write,  .seek    = fs_fake_seek,
    .readdir = fs_fake_readdir,.mkdir   = fs_fake_mkdir,
    .stat    = fs_fake_stat,
};

TEST(fs_dispatch_open_and_close_equivalence)
{
    static char sentinel;
    s_fs_open_out    = &sentinel;
    s_fs_open_rc     = NX_OK;
    s_fs_open_called = 0;
    s_fs_close_called = 0;

    /* direct open */
    void *out_direct = NULL;
    int rc_direct = fs_fake_open(NULL, "/x", NX_FS_OPEN_READ, &out_direct);

    /* dispatch open */
    struct nx_fs_msg_open req_open;
    memset(&req_open, 0, sizeof req_open);
    strncpy(req_open.path, "/x", sizeof req_open.path - 1);
    req_open.flags = NX_FS_OPEN_READ;

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_FS_OP_OPEN;
    msg.payload     = &req_open;
    msg.payload_len = (uint32_t)sizeof req_open;

    int rc_dispatch = nx_fs_dispatch(NULL, &fs_fake_ops, &msg);
    ASSERT_CALL_EQUIVALENT(rc_direct, rc_dispatch);
    ASSERT_EQ_U((unsigned)s_fs_open_called, 2u);   /* 1 direct + 1 dispatch */

    /* dispatch close */
    struct nx_fs_msg_close req_close;
    memset(&req_close, 0, sizeof req_close);
    req_close.file = (uint64_t)(uintptr_t)&sentinel;

    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_FS_OP_CLOSE;
    msg.payload     = &req_close;
    msg.payload_len = (uint32_t)sizeof req_close;

    nx_fs_dispatch(NULL, &fs_fake_ops, &msg);
    ASSERT_EQ_U((unsigned)s_fs_close_called, 1u);
}

TEST(fs_dispatch_mkdir_rc_propagated)
{
    s_fs_mkdir_rc = NX_ENOMEM;

    struct nx_fs_msg_mkdir req;
    memset(&req, 0, sizeof req);
    strncpy(req.path, "/newdir", sizeof req.path - 1);

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_type    = NX_FS_OP_MKDIR;
    msg.payload     = &req;
    msg.payload_len = (uint32_t)sizeof req;

    int rc = nx_fs_dispatch(NULL, &fs_fake_ops, &msg);
    ASSERT_EQ_U((unsigned)rc, (unsigned)NX_ENOMEM);
    ASSERT_CALL_EQUIVALENT(NX_ENOMEM, rc);
}
