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

    /* 아래 네 개도 전부 선택 사항이다(NULL 이면 Nunsupported) - 읽기 전용 파일시스템
     * (fat16 등)은 그냥 NULL 로 둔다. */
    Nstatus (*create)(vnode_t *dir, const char *name, u32 mode, vnode_t **out_vnode);  /* 새 파일 */
    Nstatus (*mkdir)(vnode_t *dir, const char *name, u32 mode, vnode_t **out_vnode);   /* 새 디렉터리 */
    Nstatus (*unlink)(vnode_t *dir, const char *name);   /* 파일 제거, 또는 빈 디렉터리 제거(rmdir 겸용) */
    /* index 번째(0부터) 자식의 이름을 name_out 에 채운다. index 가 자식 수 이상이면
     * NnotFound 로 "더 이상 없음"을 알린다. */
    Nstatus (*readdir)(vnode_t *dir, u64 index, char *name_out, usize name_out_max, u32 *type_out);
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
 * path 의 부모 디렉터리 아래에 새 파일/디렉터리를 만든다. 부모까지의 경로는 이미
 * 존재해야 한다(mkdir -p 처럼 중간 디렉터리를 자동으로 만들지 않는다).
 * 대상 파일시스템이 create/mkdir 을 지원하지 않으면 Nunsupported.
 */
Nstatus vfs_create(const char *path, u32 mode);
Nstatus vfs_mkdir(const char *path, u32 mode);

/* 파일을 지우거나(링크 수가 0 이 되면 실제로 회수), 빈 디렉터리를 지운다(rmdir 겸용).
 * 디렉터리가 비어있지 않으면 파일시스템 드라이버가 Nunsupported 나 다른 오류를 돌려준다. */
Nstatus vfs_unlink(const char *path);

/* path(디렉터리) 의 index 번째(0부터) 자식 이름을 name_out 에 채운다.
 * index 가 자식 수 이상이면 NnotFound ("더 이상 없음"). 호출자는 index 를 0부터
 * 하나씩 늘려가며 NnotFound 가 나올 때까지 반복 호출해서 목록 전체를 얻는다. */
Nstatus vfs_readdir(const char *path, u64 index, char *name_out, usize name_out_max, u32 *type_out);

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
