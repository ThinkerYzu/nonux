#ifndef NONUX_CONFORMANCE_FS_H
#define NONUX_CONFORMANCE_FS_H

#include "interfaces/fs.h"

/*
 * Filesystem-driver conformance suite (slice 6.1).
 *
 * Every filesystem driver (slice 6.2's ramfs, any future tmpfs/ext2/...)
 * must pass every case below before it is allowed to be mounted by the
 * VFS layer in a production kernel.  Cases encode the universal contract
 * of `struct nx_fs_ops`; driver-specific behaviour (inline-vs-extent
 * layout, on-disk serialisation, symlink handling) is tested separately
 * in the driver's own test file.
 *
 * Usage.
 *   Define one TEST() per case wrapping each helper against a fixture
 *   that knows how to create / destroy a fresh driver instance.  The
 *   fixture's `create` MUST leave the driver in a state where `open`
 *   with CREATE on any well-formed path succeeds — tests create their
 *   own files from scratch.  Seven helpers → seven tests per driver.
 */

struct nx_fs_fixture {
    const struct nx_fs_ops *ops;
    void *(*create)(void);           /* fresh `self`; may return NULL on OOM */
    void  (*destroy)(void *self);    /* must release everything create allocated */
};

/*
 * Universal cases — must hold for every filesystem driver.
 *
 * Each helper calls `fixture->create`, exercises the op table, then
 * `fixture->destroy`.  Failures surface through ASSERT() from the host
 * test framework so the calling TEST() short-circuits on the first
 * bad assertion.
 */

void nx_conformance_fs_open_create_on_fresh_path_succeeds(
    const struct nx_fs_fixture *f);

void nx_conformance_fs_open_without_create_on_missing_path_returns_enoent(
    const struct nx_fs_fixture *f);

void nx_conformance_fs_fresh_file_reads_zero_bytes(
    const struct nx_fs_fixture *f);

void nx_conformance_fs_write_then_read_after_reopen_roundtrips(
    const struct nx_fs_fixture *f);

void nx_conformance_fs_read_past_eof_returns_zero(
    const struct nx_fs_fixture *f);

void nx_conformance_fs_two_opens_have_independent_cursors(
    const struct nx_fs_fixture *f);

void nx_conformance_fs_write_without_write_right_returns_eperm(
    const struct nx_fs_fixture *f);

/* --- slice 6.4: readdir + seek cases ------------------------------- */

void nx_conformance_fs_readdir_on_empty_fs_returns_enoent(
    const struct nx_fs_fixture *f);

void nx_conformance_fs_readdir_yields_created_files_then_enoent(
    const struct nx_fs_fixture *f);

void nx_conformance_fs_seek_set_to_zero_restarts_reads(
    const struct nx_fs_fixture *f);

void nx_conformance_fs_seek_end_returns_file_size(
    const struct nx_fs_fixture *f);

void nx_conformance_fs_seek_past_size_returns_einval(
    const struct nx_fs_fixture *f);

#endif /* NONUX_CONFORMANCE_FS_H */
