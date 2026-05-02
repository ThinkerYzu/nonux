/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/fs.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_FRAMEWORK_FS_CALL_H
#define NONUX_FRAMEWORK_FS_CALL_H

#include <stddef.h>
#include <stdint.h>

#include "interfaces/fs.h"
#include "interfaces/fs_msg.h"
#include "framework/registry.h"
#include "framework/ipc.h"
#include "framework/slot_call.h"

int nx_fs_open(struct nx_slot *slot, const char *path, uint32_t flags,
               void **out_file);
void nx_fs_close(struct nx_slot *slot, void *file);
void nx_fs_retain(struct nx_slot *slot, void *file);
int64_t nx_fs_read(struct nx_slot *slot, void *file, void *buf, size_t cap);
int64_t nx_fs_write(struct nx_slot *slot, void *file, const void *buf,
                    size_t len);
int64_t nx_fs_seek(struct nx_slot *slot, void *file, int64_t offset,
                   int whence);
int nx_fs_readdir(struct nx_slot *slot, const char *dir_path, uint32_t *cookie,
                  struct nx_fs_dirent *out);
int nx_fs_mkdir(struct nx_slot *slot, const char *path);
int nx_fs_stat(struct nx_slot *slot, const char *path, struct nx_fs_stat *out);

#endif /* NONUX_FRAMEWORK_FS_CALL_H */
