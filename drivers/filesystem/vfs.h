#pragma once

#ifndef VFS_H
#define VFS_H

#include "nyxis.h"

typedef struct vnode vnode_t;
typedef struct dentry dentry_t;
typedef struct superblock superblock_t;
typedef struct vfs_filesystem vfs_filesystem_t;
typedef struct handle handle_t;

typedef struct vfs_ops {
    Nstatus (*lookup)(vnode_t* dir, const char* name, vnode_t** out_vnode);
    Nstatus (*open)(vnode_t* vnode, u32 flags, handle_t* out_handle);
    Nstatus (*read)(handle_t* handle, void* buffer, usize size, usize* bytes_read);
    Nstatus (*write)(handle_t* handle, const void* buffer, usize size, usize* bytes_written);
    Nstatus (*close)(handle_t* handle);
} vfs_ops_t;

struct vfs_filesystem {
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
Nstatus vfs_mount(const char* path, vfs_filesystem_t* fs);
Nstatus vfs_unmount(const char* path);
Nstatus vfs_register_namespace(const char* namespace_name, const char* source_path);
Nstatus vfs_open(const char* path, u32 flags, handle_t* out_handle);
Nstatus vfs_read(handle_t* handle, void* buffer, usize size, usize* bytes_read);
Nstatus vfs_write(handle_t* handle, const void* buffer, usize size, usize* bytes_written);
Nstatus vfs_close(handle_t* handle);

vnode_t* vfs_alloc_vnode(void);
Nstatus vfs_init_vnode(
    vnode_t* vnode,
    u64 inode_id,
    u32 type,
    vfs_ops_t* ops,
    superblock_t* sb,
    void* fs_private
);
void vfs_free_vnode(vnode_t* vnode);
void* vfs_get_vnode_private(vnode_t* vnode);

#endif // VFS_H