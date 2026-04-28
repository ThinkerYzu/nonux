/*
 * Kernel-side coverage for slice 7.7b.2 — procfs.
 *
 * Fast regression guard for the synthesised process filesystem and the
 * vfs_simple mount-table that routes /proc/... to it.  The interactive
 * `ps_smoke` test (test/interactive/) already exercises the full
 * busybox-ps → procfs path end-to-end, but its 60-second-per-script
 * QEMU stdin-feed dwarfs a 2-second ktest; this file catches the
 * obvious breakage in `make test` long before the slow harness runs.
 *
 * What we cover:
 *
 *   - filesystem.proc slot + procfs binding land at boot, ACTIVE.
 *   - vfs_simple's `mount_for_path` routes "/proc" + "/proc/..."
 *     through procfs while leaving "/" + "/bin/..." on ramfs.
 *   - procfs's stat/readdir/open/read shape: /proc as DIR, /proc/0
 *     as DIR (with and without trailing slash), /proc/0/stat as FILE
 *     producing a parseable Linux-shape stat line.
 *   - render_stat output starts with "0 (kernel) R 0 " — the four
 *     fields busybox's procps_scan reads first.
 *
 * What we do *not* cover (the ps_smoke interactive test handles these):
 *   - the full musl → ash → fork(busybox ps) → readdir/openat/read
 *     dispatch chain
 *   - busybox ps's column rendering
 */

#include "ktest.h"

#include "framework/component.h"
#include "framework/process.h"
#include "framework/registry.h"
#include "interfaces/fs.h"
#include "interfaces/vfs.h"

/* ---------- Slot binding presence ----------------------------------- */

KTEST(bootstrap_binds_procfs_to_filesystem_proc_slot)
{
    struct nx_slot *s = nx_slot_lookup("filesystem.proc");
    KASSERT_NOT_NULL(s);
    KASSERT(strcmp(s->iface, "filesystem") == 0);
    KASSERT_NOT_NULL(s->active);
    KASSERT(strcmp(s->active->manifest_id, "procfs") == 0);
    KASSERT_EQ_U(s->active->state, NX_LC_ACTIVE);
}

/* ---------- Stat shape ---------------------------------------------- */

#define PROCFS_VOPS_INIT(vops_var, vself_var)                               \
    struct nx_slot *_vs = nx_slot_lookup("vfs");                            \
    KASSERT(_vs && _vs->active);                                            \
    void *vself_var = _vs->active->impl;                                    \
    const struct nx_vfs_ops *vops_var =                                     \
        (const struct nx_vfs_ops *)_vs->active->descriptor->iface_ops

KTEST(procfs_stat_root_reports_dir)
{
    PROCFS_VOPS_INIT(vops, vself);
    struct nx_fs_stat st;
    KASSERT_EQ_U(vops->stat(vself, "/proc", &st), NX_OK);
    KASSERT_EQ_U(st.kind, NX_FS_KIND_DIR);
}

KTEST(procfs_stat_pid_dir_handles_trailing_slash)
{
    /* busybox's procps_scan calls stat("/proc/<pid>/", ...) — the
     * trailing slash variant must resolve to the same DIR result as
     * "/proc/<pid>" or every process gets skipped at the UIDGID step
     * (libbb/procps.c:354). */
    PROCFS_VOPS_INIT(vops, vself);
    struct nx_fs_stat st;
    KASSERT_EQ_U(vops->stat(vself, "/proc/0", &st), NX_OK);
    KASSERT_EQ_U(st.kind, NX_FS_KIND_DIR);
    KASSERT_EQ_U(vops->stat(vself, "/proc/0/", &st), NX_OK);
    KASSERT_EQ_U(st.kind, NX_FS_KIND_DIR);
}

KTEST(procfs_stat_pid_stat_reports_file)
{
    PROCFS_VOPS_INIT(vops, vself);
    struct nx_fs_stat st;
    KASSERT_EQ_U(vops->stat(vself, "/proc/0/stat", &st), NX_OK);
    KASSERT_EQ_U(st.kind, NX_FS_KIND_FILE);
    KASSERT(st.size > 0);
}

KTEST(procfs_stat_unknown_pid_returns_enoent)
{
    PROCFS_VOPS_INIT(vops, vself);
    struct nx_fs_stat st;
    KASSERT_EQ_U(vops->stat(vself, "/proc/99999/stat", &st), NX_ENOENT);
}

/* ---------- Readdir + read of pid 0's stat -------------------------- */

KTEST(procfs_readdir_root_yields_kernel_pid_first)
{
    PROCFS_VOPS_INIT(vops, vself);
    uint32_t cookie = 0;
    struct nx_fs_dirent ent;
    KASSERT_EQ_U(vops->readdir(vself, "/proc", &cookie, &ent), NX_OK);
    /* Iteration order: kernel process (pid 0) is visited before any
     * user process by `nx_process_for_each` (slice 7.7b.2). */
    KASSERT(strcmp(ent.name, "0") == 0);
}

KTEST(procfs_open_pid0_stat_renders_kernel_R_0_prefix)
{
    /* End-to-end: vfs->open("/proc/0/stat") → procfs renders the live
     * stat line for pid 0 → vfs->read returns it.  The prefix is
     * deterministic (kernel pid 0 is ACTIVE, parent_pid 0). */
    PROCFS_VOPS_INIT(vops, vself);
    void *file = NULL;
    int rc = vops->open(vself, "/proc/0/stat", NX_VFS_OPEN_READ, &file);
    KASSERT_EQ_U(rc, NX_OK);
    KASSERT(file != NULL);

    char buf[128];
    int64_t got = vops->read(vself, file, buf, sizeof buf - 1);
    KASSERT(got > 0);
    buf[got] = '\0';
    /* busybox's procps_scan splits at the trailing ')'; the prefix up
     * to and including ") R 0 " is the contract our renderer
     * guarantees. */
    const char *want = "0 (kernel) R 0 ";
    KASSERT(strncmp(buf, want, strlen(want)) == 0);

    vops->close(vself, file);
}

/* ---------- Mount-table routing leaves ramfs alone ------------------ */

KTEST(procfs_mount_does_not_intercept_ramfs_paths)
{
    /* Sanity: a ramfs path stat (e.g. /banner from initramfs) still
     * goes to ramfs, not procfs.  Without this, a buggy `mount_for_path`
     * regex would silently route filesystem traffic to the wrong
     * driver and tests like ktest_initramfs would notice — but proving
     * the boundary explicitly here makes the contract self-documenting. */
    PROCFS_VOPS_INIT(vops, vself);
    struct nx_fs_stat st;
    KASSERT_EQ_U(vops->stat(vself, "/banner", &st), NX_OK);
    KASSERT_EQ_U(st.kind, NX_FS_KIND_FILE);
}
