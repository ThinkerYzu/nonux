/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/vfs.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_FRAMEWORK_VFS_CALL_H
#define NONUX_FRAMEWORK_VFS_CALL_H

#include <stddef.h>
#include <stdint.h>

#include "interfaces/vfs.h"
#include "interfaces/vfs_msg.h"
#include "framework/registry.h"
#include "framework/ipc.h"

/* Slice 8.0a defines `nx_slot_call_blocking` in
 * framework/slot_call.{h,c}; until that lands these wrappers
 * reference it as extern. */
extern int nx_slot_call_blocking(struct nx_slot *slot,
                                 struct nx_ipc_message *msg);

int nx_vfs_open(struct nx_slot *slot, const char *path, uint32_t flags,
                void **out_file);
void nx_vfs_close(struct nx_slot *slot, void *file);
void nx_vfs_retain(struct nx_slot *slot, void *file);
int64_t nx_vfs_read(struct nx_slot *slot, void *file, void *buf, size_t cap);
int64_t nx_vfs_write(struct nx_slot *slot, void *file, const void *buf,
                     size_t len);
int64_t nx_vfs_seek(struct nx_slot *slot, void *file, int64_t offset,
                    int whence);
int nx_vfs_readdir(struct nx_slot *slot, const char *dir_path,
                   uint32_t *cookie, struct nx_fs_dirent *out);
int nx_vfs_mkdir(struct nx_slot *slot, const char *path);
int nx_vfs_stat(struct nx_slot *slot, const char *path,
                struct nx_fs_stat *out);

#endif /* NONUX_FRAMEWORK_VFS_CALL_H */
