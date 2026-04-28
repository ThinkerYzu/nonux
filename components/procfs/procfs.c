/*
 * procfs — synthesised process filesystem (slice 7.7b.2).
 *
 * Implements the same `nx_fs_ops` contract as ramfs but every entry is
 * generated on demand from the live process table.  No on-disk storage,
 * no per-process backing — `stat` / `readdir` / `open(read)` walk
 * `framework/process.c` through the public `nx_process_for_each`
 * iterator each time they're called.
 *
 * Namespace.
 *   /proc                       — directory listing live PIDs
 *   /proc/<pid>                 — directory containing exactly "stat"
 *   /proc/<pid>/stat            — Linux-shape stat line, busybox-ps-readable
 *
 * The slice goal is busybox's stock `ps` applet — no vendored patch.
 * busybox parses /proc/<pid>/stat with `sscanf` after splitting at the
 * trailing `)` (see third_party/busybox/libbb/procps.c:380), so the
 * comm field needs parens around it and the post-`)` numeric run needs
 * at least 11 fields (state, ppid, pgid, sid, tty, tpgid, flags,
 * minflt, cminflt, majflt, cmajflt — though most are skipped via
 * `%*s` in the procps_scan format).  We emit 24 numeric fields total
 * which covers the full slow-path scan up to vsize/rss.
 *
 * Read-only.  `open` accepts read-only flag combinations; `mkdir`,
 * `retain`, and any write attempt return NX_EPERM.  Per-open state
 * caches the rendered stat content (synthesised at open time so
 * concurrent reads see a consistent snapshot rather than racing the
 * process table) plus a cursor; a small fixed-size pool keeps the
 * driver allocator-free, mirroring ramfs.
 *
 * Mounted under `/proc` by vfs_simple's slice-7.7b.2 mount table.  The
 * vfs forwards the *full* path (including the `/proc` prefix) to
 * procfs ops because we want procfs to recognise "/proc" itself as
 * the root directory entry — having vfs_simple strip the prefix would
 * leave us with the empty string, which collides with our existing
 * absolute-path-only convention.  Drivers that prefer the stripped
 * form can opt in with a future "strip_mount" flag in the mount table.
 */

#include "framework/component.h"
#include "framework/registry.h"
#include "framework/process.h"
#include "interfaces/fs.h"

#include <stddef.h>
#include <stdint.h>

#if __STDC_HOSTED__
#include <string.h>
#else
#include "core/lib/lib.h"
#endif

#define PROCFS_MAX_OPEN     8u
#define PROCFS_STAT_BUFSZ   384u   /* one stat line, room for 24 numeric
                                    * fields + 16-char comm */

struct procfs_open {
    int    in_use;
    int    refs;
    size_t size;
    size_t cursor;
    char   buf[PROCFS_STAT_BUFSZ];
};

struct procfs_state {
    struct procfs_open opens[PROCFS_MAX_OPEN];

    /* Lifecycle counters for test introspection — same shape as every
     * other component so ktest can prove each verb fired. */
    unsigned init_called;
    unsigned enable_called;
    unsigned disable_called;
    unsigned destroy_called;
};

/* ---------- Path parsing ---------------------------------------------- */
/*
 * Path classification helpers.  All paths are absolute and start with
 * "/proc".  We accept the trailing slash variant ("/proc/") as the
 * same as "/proc" because some libc paths normalise to the trailing
 * form; busybox doesn't currently emit it but the cost is one branch.
 */

static int procfs_path_is_root(const char *path)
{
    if (path[0] != '/' || path[1] != 'p' || path[2] != 'r' ||
        path[3] != 'o' || path[4] != 'c') return 0;
    if (path[5] == '\0') return 1;
    if (path[5] == '/' && path[6] == '\0') return 1;
    return 0;
}

/* If `path` matches "/proc/<digits>" (optionally followed by "/<rest>"),
 * write the parsed pid into *out_pid and return a pointer to the byte
 * after the digits (either '\0' or '/').  Returns NULL on mismatch. */
static const char *procfs_parse_pid(const char *path, uint32_t *out_pid)
{
    if (path[0] != '/' || path[1] != 'p' || path[2] != 'r' ||
        path[3] != 'o' || path[4] != 'c' || path[5] != '/') return NULL;

    const char *p = path + 6;
    if (*p < '0' || *p > '9') return NULL;

    uint32_t pid = 0;
    while (*p >= '0' && *p <= '9') {
        uint32_t next = pid * 10u + (uint32_t)(*p - '0');
        if (next < pid) return NULL;     /* overflow */
        pid = next;
        p++;
    }
    *out_pid = pid;
    return p;
}

/* ---------- Process iteration helpers --------------------------------- */
/*
 * procfs needs three indexable views of the live process table:
 *   - find the Nth process (cookie-driven readdir)
 *   - look up by pid (stat / open of /proc/<pid>/...)
 *   - check whether a pid exists (stat of /proc/<pid> as DIR)
 *
 * `nx_process_lookup_by_pid` covers the second; the first is
 * implemented on top of `nx_process_for_each` via a "stop at index N"
 * callback.  O(n) per lookup, O(n²) per directory listing, fine for
 * n ≤ 129.
 */

struct find_nth_ctx {
    uint32_t                target_idx;
    uint32_t                current_idx;
    struct nx_process      *result;
};

static int find_nth_cb(struct nx_process *p, void *ctx)
{
    struct find_nth_ctx *c = ctx;
    if (c->current_idx == c->target_idx) {
        c->result = p;
        return 1;                        /* stop iteration */
    }
    c->current_idx++;
    return 0;
}

static struct nx_process *procfs_find_nth(uint32_t idx)
{
    struct find_nth_ctx ctx = { .target_idx = idx, .current_idx = 0,
                                .result = NULL };
    nx_process_for_each(find_nth_cb, &ctx);
    return ctx.result;
}

/* ---------- Stat-line rendering --------------------------------------- */
/*
 * Append `n` to *buf, advancing *off.  Decimal, no leading zeros (same
 * shape as Linux's /proc/<pid>/stat).  Caller-bounded by `cap`; on
 * overflow we silently truncate — busybox's sscanf will then see fewer
 * than 11 fields and skip the entry, but we never overrun.
 */
static void procfs_emit_u(char *buf, size_t cap, size_t *off, uint64_t n)
{
    if (*off >= cap) return;
    char tmp[24];
    int  len = 0;
    if (n == 0) { tmp[len++] = '0'; }
    else {
        while (n) { tmp[len++] = (char)('0' + (n % 10u)); n /= 10u; }
    }
    while (len > 0 && *off < cap) {
        buf[*off] = tmp[--len];
        (*off)++;
    }
}

static void procfs_emit_char(char *buf, size_t cap, size_t *off, char c)
{
    if (*off < cap) { buf[*off] = c; (*off)++; }
}

static void procfs_emit_str(char *buf, size_t cap, size_t *off,
                            const char *s, size_t slen)
{
    for (size_t i = 0; i < slen && *off < cap; i++) {
        buf[*off] = s[i]; (*off)++;
    }
}

/*
 * Render `p`'s /proc/<pid>/stat line into `out[]` (capacity `cap`).
 * Returns the byte count actually written (≤ cap).  Format is the
 * Linux /proc(5) stat layout, truncated to the 24 fields busybox
 * `procps_scan`'s slow path actually consumes:
 *
 *   pid (comm) state ppid pgid sid tty_nr tpgid flags
 *   minflt cminflt majflt cmajflt utime stime cutime cstime
 *   priority nice num_threads itrealvalue starttime vsize rss
 *
 * Everything past `state`/`ppid` is reported as 0 — v1 doesn't track
 * any of it.  busybox tolerates the zeros: ps prints `0` in the VSZ
 * column and computes elapsed time as 0.
 */
static size_t procfs_render_stat(struct nx_process *p, char *out, size_t cap)
{
    size_t off = 0;

    procfs_emit_u(out, cap, &off, p->pid);
    procfs_emit_str(out, cap, &off, " (", 2);

    /* comm — bracketed by ( and ).  Truncate to NX_PROCESS_NAME_MAX-1
     * (16 - 1 = 15) since busybox's BUILD_BUG_ON requires `comm[]` to
     * be at least 16 chars including NUL. */
    size_t nlen = 0;
    while (nlen < NX_PROCESS_NAME_MAX - 1 && p->name[nlen] != '\0') nlen++;
    if (nlen == 0) {
        /* Empty name → "?" so busybox's strchr('(') / strrchr(')') see a
         * non-empty inside. */
        procfs_emit_char(out, cap, &off, '?');
    } else {
        procfs_emit_str(out, cap, &off, p->name, nlen);
    }
    procfs_emit_str(out, cap, &off, ") ", 2);

    /* state: R = running (ACTIVE), Z = zombie (EXITED).  busybox's ps
     * displays this verbatim as the STAT column. */
    char state_ch = (p->state == NX_PROCESS_STATE_EXITED) ? 'Z' : 'R';
    procfs_emit_char(out, cap, &off, state_ch);
    procfs_emit_char(out, cap, &off, ' ');

    /* ppid */
    procfs_emit_u(out, cap, &off, p->parent_pid);

    /* Twenty-one trailing zeros: pgid sid tty tpgid flags minflt cminflt
     * majflt cmajflt utime stime cutime cstime priority nice
     * num_threads itrealvalue starttime vsize rss + (one extra so the
     * `n < 11` short-read guard always passes with margin). */
    for (int i = 0; i < 21; i++) {
        procfs_emit_str(out, cap, &off, " 0", 2);
    }

    procfs_emit_char(out, cap, &off, '\n');
    return off;
}

/* ---------- nx_fs_ops -------------------------------------------------- */

static struct procfs_open *procfs_alloc_open(struct procfs_state *s)
{
    for (unsigned i = 0; i < PROCFS_MAX_OPEN; i++) {
        if (!s->opens[i].in_use) {
            s->opens[i].in_use = 1;
            return &s->opens[i];
        }
    }
    return NULL;
}

static int procfs_op_open(void *self, const char *path, uint32_t flags,
                          void **out_file)
{
    if (!self || !path || !out_file) return NX_EINVAL;
    if (path[0] == '\0') return NX_EINVAL;

    /* Read-only filesystem: refuse any write/create/append flag.  Read
     * may be requested explicitly (NX_FS_OPEN_READ) or implicitly by
     * passing 0 (busybox's open() with no O_WRONLY drops to flags=0). */
    if (flags & (NX_FS_OPEN_WRITE | NX_FS_OPEN_CREATE | NX_FS_OPEN_APPEND))
        return NX_EPERM;

    /* Only /proc/<pid>/stat is openable as a file in v1.  Directory
     * paths are routed through HANDLE_DIR by the syscall layer's stat
     * probe, so this op never sees them in the live build; but defend
     * against it (the host fake_fs path may call open on a dir). */
    uint32_t pid;
    const char *tail = procfs_parse_pid(path, &pid);
    if (!tail) return NX_ENOENT;

    /* Match exactly "/stat" (no other files exist under /proc/<pid>). */
    if (tail[0] != '/' || tail[1] != 's' || tail[2] != 't' ||
        tail[3] != 'a' || tail[4] != 't' || tail[5] != '\0')
        return NX_ENOENT;

    struct nx_process *p = nx_process_lookup_by_pid(pid);
    if (!p) return NX_ENOENT;

    struct procfs_state *s = self;
    struct procfs_open  *op = procfs_alloc_open(s);
    if (!op) return NX_ENOMEM;
    op->refs   = 1;
    op->cursor = 0;
    op->size   = procfs_render_stat(p, op->buf, PROCFS_STAT_BUFSZ);

    *out_file = op;
    return NX_OK;
}

static void procfs_op_close(void *self, void *file)
{
    (void)self;
    if (!file) return;
    struct procfs_open *op = file;
    if (op->refs > 0 && --op->refs > 0) return;
    op->in_use = 0;
    op->size   = 0;
    op->cursor = 0;
}

static void procfs_op_retain(void *self, void *file)
{
    (void)self;
    if (!file) return;
    struct procfs_open *op = file;
    op->refs++;
}

static int64_t procfs_op_read(void *self, void *file, void *buf, size_t cap)
{
    (void)self;
    if (!file) return NX_EINVAL;
    if (cap == 0) return 0;
    if (!buf) return NX_EINVAL;

    struct procfs_open *op = file;
    size_t remain = (op->cursor < op->size) ? op->size - op->cursor : 0;
    size_t n      = remain < cap ? remain : cap;
    if (n > 0) memcpy(buf, op->buf + op->cursor, n);
    op->cursor += n;
    return (int64_t)n;
}

static int64_t procfs_op_write(void *self, void *file,
                               const void *buf, size_t len)
{
    (void)self; (void)file; (void)buf; (void)len;
    return NX_EPERM;        /* read-only filesystem */
}

static int64_t procfs_op_seek(void *self, void *file,
                              int64_t offset, int whence)
{
    (void)self;
    if (!file) return NX_EINVAL;
    struct procfs_open *op = file;

    int64_t base;
    switch (whence) {
    case NX_FS_SEEK_SET: base = 0;                   break;
    case NX_FS_SEEK_CUR: base = (int64_t)op->cursor; break;
    case NX_FS_SEEK_END: base = (int64_t)op->size;   break;
    default:             return NX_EINVAL;
    }
    int64_t new_pos = base + offset;
    if (new_pos < 0 || (uint64_t)new_pos > op->size) return NX_EINVAL;
    op->cursor = (size_t)new_pos;
    return new_pos;
}

/*
 * Readdir for procfs.
 *
 *   "/proc"           → emit each live process as "<pid>" basename;
 *                       cookie indexes into the iteration order
 *                       returned by `nx_process_for_each`.
 *   "/proc/<pid>"     → emit a single entry "stat".  cookie 0 → "stat",
 *                       cookie 1 → ENOENT.
 *   anything else     → ENOENT.
 */
static int procfs_op_readdir(void *self, const char *dir_path,
                             uint32_t *cookie, struct nx_fs_dirent *out)
{
    if (!self || !dir_path || !cookie || !out) return NX_EINVAL;
    if (dir_path[0] != '/') return NX_EINVAL;

    if (procfs_path_is_root(dir_path)) {
        struct nx_process *p = procfs_find_nth(*cookie);
        if (!p) { return NX_ENOENT; }
        *cookie += 1;

        /* Render p->pid as decimal into out->name.  PIDs are 32-bit
         * unsigned; longest decimal repr is 10 digits which fits the
         * 64-byte name buffer easily. */
        char tmp[16];
        int  len = 0;
        uint32_t v = p->pid;
        if (v == 0) { tmp[len++] = '0'; }
        else { while (v) { tmp[len++] = (char)('0' + (v % 10u)); v /= 10u; } }
        out->name_len = (uint32_t)len;
        for (int i = 0; i < len; i++) out->name[i] = tmp[len - 1 - i];
        out->name[len] = '\0';
        return NX_OK;
    }

    uint32_t pid;
    const char *tail = procfs_parse_pid(dir_path, &pid);
    if (tail && (tail[0] == '\0' ||
                 (tail[0] == '/' && tail[1] == '\0'))) {
        /* /proc/<pid> (with or without trailing slash): list its
         * single child. */
        if (!nx_process_lookup_by_pid(pid)) return NX_ENOENT;
        if (*cookie != 0) return NX_ENOENT;
        out->name_len = 4;
        out->name[0] = 's'; out->name[1] = 't';
        out->name[2] = 'a'; out->name[3] = 't';
        out->name[4] = '\0';
        *cookie = 1;
        return NX_OK;
    }

    return NX_ENOENT;
}

static int procfs_op_mkdir(void *self, const char *path)
{
    (void)self; (void)path;
    return NX_EPERM;            /* read-only filesystem */
}

static int procfs_op_stat(void *self, const char *path,
                          struct nx_fs_stat *out)
{
    if (!self || !path || !out) return NX_EINVAL;
    if (path[0] != '/') return NX_EINVAL;

    if (procfs_path_is_root(path)) {
        out->kind = NX_FS_KIND_DIR;
        out->size = 0;
        return NX_OK;
    }

    uint32_t pid;
    const char *tail = procfs_parse_pid(path, &pid);
    if (!tail) return NX_ENOENT;

    if (!nx_process_lookup_by_pid(pid)) return NX_ENOENT;

    /* "/proc/<pid>" or "/proc/<pid>/" — directory containing "stat".
     * busybox's procps_scan calls `stat("/proc/%u/", &sb)` (with the
     * trailing slash) to read uid/gid; without the trailing-slash
     * branch it'd see ENOENT and skip every process. */
    if (tail[0] == '\0' || (tail[0] == '/' && tail[1] == '\0')) {
        out->kind = NX_FS_KIND_DIR;
        out->size = 0;
        return NX_OK;
    }

    /* "/proc/<pid>/stat" — the only regular file. */
    if (tail[0] == '/' && tail[1] == 's' && tail[2] == 't' &&
        tail[3] == 'a' && tail[4] == 't' && tail[5] == '\0') {
        out->kind = NX_FS_KIND_FILE;
        /* size is best-effort: render once into a throwaway buffer to
         * report the same byte count callers will see from read().
         * busybox doesn't consult st_size before reading so the cost
         * is paid even though it's wasted; a future slice could cache
         * the size on the process struct or report 0 (POSIX permits
         * st_size == 0 for special files). */
        struct nx_process *p = nx_process_lookup_by_pid(pid);
        char tmp[PROCFS_STAT_BUFSZ];
        out->size = (int64_t)procfs_render_stat(p, tmp, PROCFS_STAT_BUFSZ);
        return NX_OK;
    }

    return NX_ENOENT;
}

const struct nx_fs_ops procfs_fs_ops = {
    .open    = procfs_op_open,
    .close   = procfs_op_close,
    .retain  = procfs_op_retain,
    .read    = procfs_op_read,
    .write   = procfs_op_write,
    .seek    = procfs_op_seek,
    .readdir = procfs_op_readdir,
    .mkdir   = procfs_op_mkdir,
    .stat    = procfs_op_stat,
};

/* ---------- Component lifecycle --------------------------------------- */

static int procfs_init(void *self)
{
    struct procfs_state *s = self;
    for (unsigned i = 0; i < PROCFS_MAX_OPEN; i++) s->opens[i].in_use = 0;
    s->init_called++;
    return NX_OK;
}

static int procfs_enable(void *self)
{
    struct procfs_state *s = self;
    s->enable_called++;
    return NX_OK;
}

static int procfs_disable(void *self)
{
    struct procfs_state *s = self;
    s->disable_called++;
    return NX_OK;
}

static void procfs_destroy(void *self)
{
    struct procfs_state *s = self;
    s->destroy_called++;
}

const struct nx_component_ops procfs_component_ops = {
    .init    = procfs_init,
    .enable  = procfs_enable,
    .disable = procfs_disable,
    .destroy = procfs_destroy,
    /* No pause_hook: spawns_threads is false in the manifest.
     * No handle_msg: procfs is consumed through iface_ops by vfs_simple,
     * not via the IPC router. */
};

NX_COMPONENT_REGISTER_NO_DEPS_IFACE(procfs,
                                    struct procfs_state,
                                    &procfs_component_ops,
                                    &procfs_fs_ops);
