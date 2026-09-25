/*
 * nyfs_disk.h - NyFS(Nyxis File System) 온디스크 포맷 (리눅스 호스트 툴용 이식본)
 *
 * 원본: nyxis-os 저장소 drivers/filesystem/nyfs/nyfs_disk.h (GPL-3.0-or-later).
 * 이 파일은 그 헤더의 "디스크 바이트 배치" 정의를 필드 하나 순서 하나까지
 * 그대로 옮긴 것이다 - 절대 임의로 필드를 추가/삭제/재배치하지 않는다.
 * 바뀐 것은 오직 include 대상뿐이다: 커널 전용 include/types.h 대신 이
 * 프로젝트의 nyfs_host.h(리눅스 호스트용 동등 타입)를 쓴다.
 *
 * 왜 커널 헤더를 그대로 #include 하지 않고 복사했는가: 커널 include/types.h
 * 는 프리스탠딩 환경(NTBLI, freestanding 전제) 코드를 포함하고 있어 일반
 * 리눅스 유저스페이스 빌드에 그대로 끌어올 수 없다. 온디스크 레이아웃을
 * "값으로" 동기화하는 대신, u8/u16/u32/u64/pack/정적 단언 매크로가 정확히
 * 같은 의미가 되도록 nyfs_host.h 를 맞춰 두고 이 파일은 그 위에서 커널
 * 원본과 바이트 단위로 동일한 구조체를 다시 선언한다.
 *
 * 커널 원본이 바뀌면(필드 추가/NYFS_VERSION 상승 등) 이 파일도 반드시 같이
 * 갱신해야 한다 - 그러지 않으면 mkfs.nyfs/nyfs-fuse 가 만든 볼륨을 커널이
 * 못 읽거나(혹은 그 반대) 조용히 데이터가 깨질 수 있다.
 */
#ifndef NYFS_DISK_H
#define NYFS_DISK_H

#include "nyfs_host.h"

/* ------------------------------------------------------------------ */
/* 기본 타입                                                           */
/* ------------------------------------------------------------------ */

typedef u64 nyfs_blkno_t;
typedef u64 nyfs_ino_t;

#define NYFS_BLKNO_NONE  ((nyfs_blkno_t)0)
#define NYFS_INO_NONE    ((nyfs_ino_t)0)
#define NYFS_ROOT_INO    ((nyfs_ino_t)1)

/* ------------------------------------------------------------------ */
/* 매직 / 버전                                                         */
/* ------------------------------------------------------------------ */

#define NYFS_MAGIC    0x53465949U
#define NYFS_VERSION  1U

#define NYFS_DIRHASH_MAGIC  0x48524944U
#define NYFS_EXTENT_MAGIC   0x42545845U

/* ------------------------------------------------------------------ */
/* 블록 크기                                                           */
/* ------------------------------------------------------------------ */

#define NYFS_BLOCK_SIZE_SHIFT_MIN  9U   /* 512 B */
#define NYFS_BLOCK_SIZE_SHIFT_MAX  16U  /* 64 KiB */

/* ------------------------------------------------------------------ */
/* 블록 할당자 전략                                                     */
/* ------------------------------------------------------------------ */

#define NYFS_ALLOC_BITMAP  0U

/* ------------------------------------------------------------------ */
/* 슈퍼블록 (블록 0)                                                    */
/* ------------------------------------------------------------------ */

typedef struct nyfs_superblock {
    u32 magic;
    u32 version;
    u32 superblock_size;
    u8  block_size_shift;
    u8  alloc_algorithm;
    u8  reserved0[2];

    char label[32];

    u64 total_blocks;
    u64 free_blocks;

    u64 inode_table_start;
    u64 inode_count;
    u64 free_inode_count;

    u64 alloc_meta_start;
    u64 alloc_meta_blocks;

    u64 inode_bitmap_start;

    u64 journal_start;
    u64 journal_blocks;
    u16 journal_version;
    u16 reserved1;
    u32 reserved2;

    u64 mount_count;
    u64 last_mount_time_ns;
    u64 last_write_time_ns;

    u32 checksum;
    u8  reserved3[92];
} pack nyfs_superblock_t;

NYFS_STATIC_ASSERT(nyfs_superblock_size_is_256, sizeof(nyfs_superblock_t) == 256);

/* ------------------------------------------------------------------ */
/* 권한: UNIX 모드                                                      */
/* ------------------------------------------------------------------ */

#define NYFS_MODE_ISUID   0x0800U
#define NYFS_MODE_ISGID   0x0400U
#define NYFS_MODE_STICKY  0x0200U
#define NYFS_MODE_IRUSR   0x0100U
#define NYFS_MODE_IWUSR   0x0080U
#define NYFS_MODE_IXUSR   0x0040U
#define NYFS_MODE_IRGRP   0x0020U
#define NYFS_MODE_IWGRP   0x0010U
#define NYFS_MODE_IXGRP   0x0008U
#define NYFS_MODE_IROTH   0x0004U
#define NYFS_MODE_IWOTH   0x0002U
#define NYFS_MODE_IXOTH   0x0001U

/* ------------------------------------------------------------------ */
/* 권한: NTFS 스타일 ACE(Access Control Entry)                          */
/* ------------------------------------------------------------------ */

#define NYFS_ACE_READ_DATA       0x00000001U
#define NYFS_ACE_WRITE_DATA      0x00000002U
#define NYFS_ACE_APPEND_DATA     0x00000004U
#define NYFS_ACE_DELETE_CHILD    0x00000008U
#define NYFS_ACE_EXECUTE         0x00000010U
#define NYFS_ACE_DELETE          0x00000020U
#define NYFS_ACE_READ_ATTR       0x00000040U
#define NYFS_ACE_WRITE_ATTR      0x00000080U
#define NYFS_ACE_READ_ACL        0x00000100U
#define NYFS_ACE_WRITE_ACL       0x00000200U
#define NYFS_ACE_TAKE_OWNERSHIP  0x00000400U

#define NYFS_ACE_GENERIC_READ \
    (NYFS_ACE_READ_DATA | NYFS_ACE_READ_ATTR | NYFS_ACE_READ_ACL)
#define NYFS_ACE_GENERIC_WRITE \
    (NYFS_ACE_WRITE_DATA | NYFS_ACE_APPEND_DATA | NYFS_ACE_WRITE_ATTR)
#define NYFS_ACE_GENERIC_EXECUTE \
    (NYFS_ACE_EXECUTE | NYFS_ACE_READ_ATTR)
#define NYFS_ACE_GENERIC_ALL  0x7FFFFFFFU

#define NYFS_ACE_TYPE_ALLOW  0U
#define NYFS_ACE_TYPE_DENY   1U

#define NYFS_ACE_FLAG_GROUP         0x01U
#define NYFS_ACE_FLAG_INHERIT_FILE  0x02U
#define NYFS_ACE_FLAG_INHERIT_DIR   0x04U
#define NYFS_ACE_FLAG_INHERIT_ONLY  0x08U
#define NYFS_ACE_FLAG_NO_PROPAGATE  0x10U

#define NYFS_ID_EVERYONE  0xFFFFFFFFU

typedef struct nyfs_ace {
    u32 id;
    u32 access_mask;
    u8  type;
    u8  flags;
    u16 reserved;
} pack nyfs_ace_t;

NYFS_STATIC_ASSERT(nyfs_ace_size_is_12, sizeof(nyfs_ace_t) == 12);

#define NYFS_ACL_INLINE_COUNT  4U

/* ------------------------------------------------------------------ */
/* inode 종류                                                          */
/* ------------------------------------------------------------------ */

#define NYFS_INO_TYPE_FILE     1U
#define NYFS_INO_TYPE_DIR      2U
#define NYFS_INO_TYPE_SYMLINK  3U
#define NYFS_INO_TYPE_DEVICE   4U

/* ------------------------------------------------------------------ */
/* inode (고정 256바이트)                                               */
/* ------------------------------------------------------------------ */

typedef struct nyfs_inode {
    u64 ino;
    u16 type;
    u16 unix_mode;
    u32 uid;
    u32 gid;
    u32 link_count;

    u16 ace_count;
    u16 reserved0;
    nyfs_ace_t ace[NYFS_ACL_INLINE_COUNT];
    nyfs_blkno_t acl_overflow_block;
    u32 acl_overflow_count;
    u32 reserved1;

    u64 size;
    nyfs_blkno_t data_extent_root;
    u8  data_extent_depth;
    u8  reserved2[7];

    nyfs_blkno_t dir_hash_root;
    u64 dir_entry_count;

    u64 ctime;
    u64 mtime;
    u64 atime;

    u64 last_txn_id;

    u32 checksum;
    u8  reserved3[88];
} pack nyfs_inode_t;

NYFS_STATIC_ASSERT(nyfs_inode_size_is_256, sizeof(nyfs_inode_t) == 256);

/* ------------------------------------------------------------------ */
/* 데이터 extent                                                       */
/* ------------------------------------------------------------------ */

typedef struct nyfs_extent {
    u64 start_block;
    u64 length;
} pack nyfs_extent_t;

NYFS_STATIC_ASSERT(nyfs_extent_size_is_16, sizeof(nyfs_extent_t) == 16);

typedef struct nyfs_extent_block_header {
    u32 magic;
    u32 extent_count;
    u64 next_block;
} pack nyfs_extent_block_header_t;

NYFS_STATIC_ASSERT(nyfs_extent_block_header_size_is_16, sizeof(nyfs_extent_block_header_t) == 16);

/* ------------------------------------------------------------------ */
/* 디렉터리 자식 해시테이블                                              */
/* ------------------------------------------------------------------ */

typedef struct nyfs_dirhash_header {
    u32 magic;
    u32 bucket_count;
    u64 entry_count;
    u64 free_offset;
    u32 checksum;
    u32 reserved0;
} pack nyfs_dirhash_header_t;

NYFS_STATIC_ASSERT(nyfs_dirhash_header_size_is_32, sizeof(nyfs_dirhash_header_t) == 32);

#define NYFS_DIRENT_TYPE_UNKNOWN  0U
#define NYFS_DIRENT_TYPE_FILE     1U
#define NYFS_DIRENT_TYPE_DIR      2U
#define NYFS_DIRENT_TYPE_SYMLINK  3U
#define NYFS_DIRENT_TYPE_DEVICE   4U

NYFS_STATIC_ASSERT(dirent_type_matches_ino_type_file, NYFS_DIRENT_TYPE_FILE == NYFS_INO_TYPE_FILE);
NYFS_STATIC_ASSERT(dirent_type_matches_ino_type_dir, NYFS_DIRENT_TYPE_DIR == NYFS_INO_TYPE_DIR);

typedef struct nyfs_dirent {
    u64 ino;
    u64 next_offset;
    u32 name_hash;
    u16 name_len;
    u8  file_type;
    u8  reserved0;
} pack nyfs_dirent_t;

NYFS_STATIC_ASSERT(nyfs_dirent_size_is_24, sizeof(nyfs_dirent_t) == 24);

#define NYFS_NAME_MAX  255U

#endif /* NYFS_DISK_H */
