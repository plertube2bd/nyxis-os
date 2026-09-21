/*
 * vfs.h - 가상 파일시스템 (VFS)
 *
 * [수정 이력 요약]
 *  - 'typedef struct handle handle_t;' 를 두 번 typedef 하던 것 정리 (C89/C99 에서는 오류).
 *  - vfs_filesystem_t 의 'private' 필드를 'priv' 로 변경 ('private' 는 C++ 예약어라
 *    이 헤더를 C++ 유저랜드가 포함하면 컴파일이 깨진다).
 *  - '#pragma once' 제거.
 */
#ifndef VFS_H
#define VFS_H

#include "nyxis.h"

typedef struct vnode vnode_t;
typedef struct dentry dentry_t;
typedef struct superblock superblock_t;
typedef struct vfs_filesystem vfs_filesystem_t;
typedef struct handle handle_t;

/* 파일 정보 (NxStat 의 바탕). type: 1 = 파일, 2 = 디렉터리 */
typedef struct vfs_stat {
    u64 size;
    u32 type;
    u32 reserved;
} vfs_stat_t;

typedef struct vfs_ops {
    Nstatus (*lookup)(vnode_t *dir, const char *name, vnode_t **out_vnode);
    Nstatus (*open)(vnode_t *vnode, u32 flags, handle_t *out_handle);
    Nstatus (*read)(handle_t *handle, void *buffer, usize size, usize *bytes_read);
    Nstatus (*write)(handle_t *handle, const void *buffer, usize size, usize *bytes_written);
    Nstatus (*close)(handle_t *handle);
    Nstatus (*stat)(vnode_t *vnode, vfs_stat_t *out);   /* 선택 사항 (NULL 이면 Nunsupported) */
} vfs_ops_t;

struct vfs_filesystem {
    const char *name;
    superblock_t *sb;
    vfs_ops_t *ops;
    vnode_t *root;
    void *priv;
};

struct handle {
    vnode_t *vnode;
    u64 offset;
    u32 flags;
    void *fs_private;
    atomic_t refcnt;
    spinlock_t lock;
};

Nstatus vfs_init(void);
Nstatus vfs_mount(const char *path, vfs_filesystem_t *fs);
Nstatus vfs_unmount(const char *path);
Nstatus vfs_register_namespace(const char *namespace_name, const char *source_path);
Nstatus vfs_open(const char *path, u32 flags, handle_t *out_handle);
Nstatus vfs_read(handle_t *handle, void *buffer, usize size, usize *bytes_read);
Nstatus vfs_write(handle_t *handle, const void *buffer, usize size, usize *bytes_written);
Nstatus vfs_close(handle_t *handle);

/* 열린 핸들이 가리키는 파일의 정보를 얻는다 (경로가 아니라 핸들 기준이라 경로 경쟁이 없다) */
Nstatus vfs_fstat(handle_t *handle, vfs_stat_t *out);

/*
 * src 와 같은 파일을 새로 열어 dst 에 넣는다. 오프셋/플래그는 복사되지만 이후에는 서로 독립이다.
 * (파일시스템 드라이버 내부 상태를 공유하면 한쪽을 닫을 때 다른 쪽이 깨지므로 항상 새로 연다)
 */
Nstatus vfs_reopen(const handle_t *src, handle_t *dst);

vnode_t *vfs_alloc_vnode(void);
Nstatus vfs_init_vnode(
    vnode_t *vnode,
    u64 inode_id,
    u32 type,
    vfs_ops_t *ops,
    superblock_t *sb,
    void *fs_private
);
void vfs_free_vnode(vnode_t *vnode);
void *vfs_get_vnode_private(vnode_t *vnode);

#endif /* VFS_H */
