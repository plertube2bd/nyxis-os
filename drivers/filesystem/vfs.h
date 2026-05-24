#pragma once

#ifndef VFS_H
#define VFS_H

#include "nyxis.h"

typedef struct vnode vnode_t;
typedef struct dentry dentry_t;
typedef struct superblock superblock_t;
typedef struct filesystem filesystem_t;
typedef struct handle handle_t;

typedef struct vfs_ops {
    Nstatus (*lookup)(vnode_t* dir, const char* name, vnode_t** out_vnode);
    Nstatus (*open)(vnode_t* vnode, u32 flags, handle_t* out_handle);
    Nstatus (*read)(handle_t* handle, void* buffer, usize size, usize* bytes_read);
    Nstatus (*write)(handle_t* handle, const void* buffer, usize size, usize* bytes_written);
    Nstatus (*close)(handle_t* handle);
} vfs_ops_t;

struct filesystem {
    const char* name;
    superblock_t* sb;
    vfs_ops_t* ops;
    vnode_t* root;
    void* private;
};

typedef struct handle {
    vnode_t* vnode;
    u64 offset;
    u32 flags;
    void* fs_private;
    atomic_t refcnt;
    spinlock_t lock;
} handle_t;

Nstatus vfs_init(void);
Nstatus vfs_mount(const char* path, filesystem_t* fs);
Nstatus vfs_unmount(const char* path);
Nstatus vfs_open(const char* path, u32 flags, handle_t* out_handle);
Nstatus vfs_read(handle_t* handle, void* buffer, usize size, usize* bytes_read);
Nstatus vfs_write(handle_t* handle, const void* buffer, usize size, usize* bytes_written);
Nstatus vfs_close(handle_t* handle);

#endif // VFS_H