/*
 * nyfs_disk.h - NyFS(Nyxis File System) 온디스크 포맷
 *
 * 이 헤더는 "디스크에 실제로 적히는 바이트 배치"만 정의한다. 커널 내부에서만 쓰는
 * 런타임 자료구조(락, 캐시, VFS 연결 등)는 여기 두지 않는다(다음 단계인 nyfs.h 에서 다룬다).
 * 그래서 이 헤더는 types.h 하나만 포함하며, 어떤 함수도 선언하지 않는다.
 *
 * 설계 배경(사용자와 합의된 내용 요약):
 *  - 네임스페이스/마운트 단위 관리는 이 파일시스템의 책임이 아니다. 기존
 *    drivers/filesystem/vfs.c 의 네임스페이스 테이블(g_namespaces)이 이미 그 역할을 한다.
 *    NyFS 는 그 아래에서 하나의 vfs_filesystem_t 로 마운트되는 "구체적인 온디스크 포맷"
 *    하나일 뿐이다. 그래서 이 헤더에는 별도의 "네임스페이스 테이블"이 없고, 대신
 *    슈퍼블록에 label 필드 하나만 두어 "이 볼륨을 어떤 이름으로 인식할지"의 힌트로 쓴다.
 *  - 저널링은 없다. 그러나 슈퍼블록에 journal_* 필드와 inode 에 last_txn_id 필드를
 *    지금부터 예약해 두어, 나중에 저널을 추가할 때 온디스크 레이아웃(따라서 이미 만들어진
 *    볼륨과의 호환성)을 깨지 않게 한다. journal_version == 0 이면 "저널 없음"이다.
 *  - inode 는 데이터 스트림(data_extent_root/size)과 디렉터리 자식 목록
 *    (dir_hash_root/dir_entry_count)이 완전히 독립된 필드다. 그래서 디렉터리도
 *    일반 파일처럼 read()/write() 로 내용을 가질 수 있고("cat ." 가능), 그 내용은
 *    자식 목록과 전혀 무관하다. 권한도 디렉터리 전용 비트를 새로 만들지 않고
 *    기존 rwx 를 그대로 재사용한다(순회는 x, 내용 read/write 는 r/w).
 *  - 권한은 UNIX 모드(빠른 1차 검사) 위에 NTFS 스타일 ACE 리스트를 얹은 구조다.
 *    APFS 고유의 NFSv4 스타일 ACL, SID, SACL(감사 로그)은 스코프에서 제외하기로
 *    합의했다 - UNIX 와 NTFS 만 지원하고 식별자는 32비트 UID/GID 로 통일한다.
 *
 * 온디스크 엔디안: 리틀엔디안으로 고정한다. NyxisOS 는 x86_64(리틀엔디안) 전용이라 지금은
 * 변환 코드가 필요 없지만, 이 필드 순서/폭 자체가 ABI 이므로 절대 임의로 바꾸지 않는다.
 * 포맷을 바꿔야 하면 NYFS_VERSION 을 올리고 이 헤더에 마이그레이션 방법을 주석으로 남긴다.
 *
 * 모든 구조체는 pack(=__attribute__((packed)))으로 묶어 컴파일러/아키텍처에 따른
 * 암묵적 패딩을 없앴다. 크기는 NX_STATIC_ASSERT 로 빌드 타임에 고정한다.
 */
#ifndef NYFS_DISK_H
#define NYFS_DISK_H

#include "types.h"

/* ------------------------------------------------------------------ */
/* 기본 타입                                                           */
/* ------------------------------------------------------------------ */

/* 블록 번호. 0 은 "없음"을 뜻한다. 블록 0 은 항상 슈퍼블록이 차지하므로
 * 데이터를 가리키는 포인터 필드에서 0 을 NULL 처럼 안전하게 재사용할 수 있다. */
typedef u64 nyfs_blkno_t;

/* inode 번호. 0 은 "없음". 루트 디렉터리는 항상 NYFS_ROOT_INO 다. */
typedef u64 nyfs_ino_t;

#define NYFS_BLKNO_NONE  ((nyfs_blkno_t)0)
#define NYFS_INO_NONE    ((nyfs_ino_t)0)
#define NYFS_ROOT_INO    ((nyfs_ino_t)1)

/* ------------------------------------------------------------------ */
/* 매직 / 버전                                                         */
/* ------------------------------------------------------------------ */

/* 디스크에는 'N','Y','F','S' 4바이트(리틀엔디안)로 저장된다. */
#define NYFS_MAGIC    0x53465949U
#define NYFS_VERSION  1U

/* 'D','I','R','H' (리틀엔디안) */
#define NYFS_DIRHASH_MAGIC  0x48524944U

/* 'E','X','T','B' (리틀엔디안) */
#define NYFS_EXTENT_MAGIC   0x42545845U

/* ------------------------------------------------------------------ */
/* 블록 크기                                                           */
/* ------------------------------------------------------------------ */

/* 블록 크기는 가변이며 슈퍼블록의 block_size_shift 로 정해진다.
 * 실제 크기 = 1 << block_size_shift. 512바이트 ~ 64KiB 범위만 허용한다
 * (그 밖의 값은 mkfs/mount 시 NbadFilesystem 으로 거부해야 한다). */
#define NYFS_BLOCK_SIZE_SHIFT_MIN  9U   /* 512 B */
#define NYFS_BLOCK_SIZE_SHIFT_MAX  16U  /* 64 KiB */

/* ------------------------------------------------------------------ */
/* 블록 할당자 전략                                                     */
/* ------------------------------------------------------------------ */

/* 슈퍼블록에 알고리즘 번호를 저장해 두면, 나중에 더 나은 할당 전략(예:
 * extent 크기별 free-list)을 추가해도 이미 만들어진 v1(비트맵) 볼륨을
 * 계속 올바르게 인식할 수 있다. 번호는 한번 배정하면 절대 재사용하지 않는다. */
#define NYFS_ALLOC_BITMAP  0U  /* v1: alloc_meta 영역이 블록 단위 비트맵(1비트=블록 1개) */

/* ------------------------------------------------------------------ */
/* 슈퍼블록 (블록 0)                                                    */
/* ------------------------------------------------------------------ */

typedef struct nyfs_superblock {
    u32 magic;              /* NYFS_MAGIC */
    u32 version;             /* NYFS_VERSION */
    u32 superblock_size;     /* sizeof(nyfs_superblock_t). 마운트 시 불일치하면 즉시 거부한다
                               * (포맷/커널 버전 불일치를 조용히 넘어가지 않기 위함 - 보안/안정성) */
    u8  block_size_shift;    /* 블록 크기 = 1 << block_size_shift */
    u8  alloc_algorithm;     /* NYFS_ALLOC_* */
    u8  reserved0[2];

    char label[32];          /* 사람이 읽는 볼륨 이름(NUL 종결). 필수는 아니지만 마운트 시
                               * 어떤 네임스페이스 이름으로 붙일지 고르는 힌트로 쓸 수 있다 */

    u64 total_blocks;        /* 볼륨 전체 블록 수 */
    u64 free_blocks;         /* 남은 블록 수 캐시값(진실은 항상 alloc_meta 의 내용) */

    /* inode 테이블: 고정 크기 배열이라 inode N 의 위치가
     * inode_table_start*block_size + N*sizeof(nyfs_inode_t) 로 바로 계산된다(O(1) 접근,
     * 디렉터리를 거치지 않는다는 뜻). */
    u64 inode_table_start;   /* inode 테이블 시작 블록 */
    u64 inode_count;         /* 전체 inode 슬롯 수(=이 볼륨이 가질 수 있는 최대 파일/디렉터리 수) */
    u64 free_inode_count;

    u64 alloc_meta_start;    /* 블록 할당자 메타데이터 시작 블록 (v1 이면 비트맵) */
    u64 alloc_meta_blocks;   /* 위 메타데이터가 차지하는 블록 수 */

    u64 inode_bitmap_start;  /* inode 할당 비트맵 시작 블록. inode 0 비트는 항상 1(예약)로 시작한다 */

    /*
     * 저널 예약 필드. 지금은 사용하지 않는다(저널링 없이 먼저 구현하기로 합의).
     * journal_version == 0 이면 "이 볼륨에 저널이 없다"는 뜻이고, 마운트 코드는
     * journal_start/journal_blocks 를 그냥 무시해야 한다. 나중에 저널을 추가할 때는
     * 새 journal_version 값을 정의하고 journal_start/journal_blocks 에 저널 영역을
     * 채워 넣는 것만으로 확장할 수 있다 - 슈퍼블록 크기/레이아웃은 그대로다.
     */
    u64 journal_start;
    u64 journal_blocks;
    u16 journal_version;     /* 0 = 없음 */
    u16 reserved1;
    u32 reserved2;

    u64 mount_count;         /* 마운트될 때마다 증가(디버깅/fsck 필요 여부 힌트) */
    u64 last_mount_time_ns;  /* 마지막 마운트 시각. 시계가 없으면 0 */
    u64 last_write_time_ns;  /* 마지막 쓰기 시각. 시계가 없으면 0 */

    u32 checksum;            /* 이 필드를 0으로 둔 채 계산한 슈퍼블록 전체의 CRC32 */
    u8  reserved3[92];       /* 향후 확장용. 반드시 0으로 채운다 */
} pack nyfs_superblock_t;

NX_STATIC_ASSERT(nyfs_superblock_size_is_256, sizeof(nyfs_superblock_t) == 256);

/* ------------------------------------------------------------------ */
/* 권한: UNIX 모드                                                      */
/* ------------------------------------------------------------------ */

/* 표준 POSIX mode_t 와 같은 비트 값을 쓴다(익숙함 유지). 프리스탠딩 환경이라
 * 시스템 헤더에 의존하지 않고 이 파일 안에서 직접 정의한다. */
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

/* access_mask 비트. NTFS ACCESS_MASK 를 참고했다(값 자체는 호환되지 않아도 되므로
 * NyFS 가 자체적으로 재정의했다 - 코드 차용이 아니라 개념만 참고). */
#define NYFS_ACE_READ_DATA       0x00000001U /* 파일: 읽기. 디렉터리: 자식 목록 열람(readdir) */
#define NYFS_ACE_WRITE_DATA      0x00000002U /* 파일: 쓰기. 디렉터리: 자식 추가 */
#define NYFS_ACE_APPEND_DATA     0x00000004U
#define NYFS_ACE_DELETE_CHILD    0x00000008U /* 디렉터리 안에서 자식을 삭제할 권한 */
#define NYFS_ACE_EXECUTE         0x00000010U /* 파일: 실행. 디렉터리: 통과(traverse, 옛 x비트) */
#define NYFS_ACE_DELETE          0x00000020U /* 이 항목 자체를 삭제 */
#define NYFS_ACE_READ_ATTR       0x00000040U
#define NYFS_ACE_WRITE_ATTR      0x00000080U
#define NYFS_ACE_READ_ACL        0x00000100U /* NTFS READ_CONTROL 에 대응 */
#define NYFS_ACE_WRITE_ACL       0x00000200U /* NTFS WRITE_DAC 에 대응 */
#define NYFS_ACE_TAKE_OWNERSHIP  0x00000400U /* NTFS WRITE_OWNER 에 대응 */
/* 0x00000800 ~ 0x40000000 은 예약. 0x80000000 은 부호 문제를 피하려고 쓰지 않는다. */

#define NYFS_ACE_GENERIC_READ \
    (NYFS_ACE_READ_DATA | NYFS_ACE_READ_ATTR | NYFS_ACE_READ_ACL)
#define NYFS_ACE_GENERIC_WRITE \
    (NYFS_ACE_WRITE_DATA | NYFS_ACE_APPEND_DATA | NYFS_ACE_WRITE_ATTR)
#define NYFS_ACE_GENERIC_EXECUTE \
    (NYFS_ACE_EXECUTE | NYFS_ACE_READ_ATTR)
#define NYFS_ACE_GENERIC_ALL  0x7FFFFFFFU

#define NYFS_ACE_TYPE_ALLOW  0U
#define NYFS_ACE_TYPE_DENY   1U

#define NYFS_ACE_FLAG_GROUP         0x01U /* id 필드가 gid (없으면 uid) */
#define NYFS_ACE_FLAG_INHERIT_FILE  0x02U /* 새로 생기는 파일 자식에게 상속 */
#define NYFS_ACE_FLAG_INHERIT_DIR   0x04U /* 새로 생기는 디렉터리 자식에게 상속 */
#define NYFS_ACE_FLAG_INHERIT_ONLY  0x08U /* 이 ACE 는 자신에게는 적용 안 되고 상속만 된다 */
#define NYFS_ACE_FLAG_NO_PROPAGATE  0x10U /* 한 단계만 상속하고 그 아래로는 안 내려간다 */

/* 특정 uid/gid 가 아니라 "누구에게나" 적용되는 ACE 를 표현하기 위한 특수 id 값.
 * uid/gid 로는 나올 수 없는 값(0xFFFFFFFF)을 예약해 둔다. */
#define NYFS_ID_EVERYONE  0xFFFFFFFFU

typedef struct nyfs_ace {
    u32 id;             /* uid 또는 gid (flags 의 NYFS_ACE_FLAG_GROUP 로 구분) */
    u32 access_mask;     /* NYFS_ACE_* 비트 조합 */
    u8  type;             /* NYFS_ACE_TYPE_ALLOW / NYFS_ACE_TYPE_DENY */
    u8  flags;            /* NYFS_ACE_FLAG_* 비트 조합 */
    u16 reserved;         /* 0 이어야 한다 */
} pack nyfs_ace_t;

NX_STATIC_ASSERT(nyfs_ace_size_is_12, sizeof(nyfs_ace_t) == 12);

/* inode 안에 인라인으로 들어가는 ACE 슬롯 수. 대부분의 파일은 ACL 이 필요 없거나
 * 몇 개면 충분하므로, 이 안에서 해결되면 별도 블록을 읽지 않아도 된다(빠른 경로). */
#define NYFS_ACL_INLINE_COUNT  4U

/* ------------------------------------------------------------------ */
/* inode 종류                                                          */
/* ------------------------------------------------------------------ */

#define NYFS_INO_TYPE_FILE     1U
#define NYFS_INO_TYPE_DIR      2U
#define NYFS_INO_TYPE_SYMLINK  3U
#define NYFS_INO_TYPE_DEVICE   4U

/* ------------------------------------------------------------------ */
/* inode (고정 256바이트, 고정폭 배열 - O(1) 인덱싱)                       */
/* ------------------------------------------------------------------ */

typedef struct nyfs_inode {
    u64 ino;               /* 자기 자신의 inode 번호(교차 검증/오류 탐지용) */
    u16 type;                /* NYFS_INO_TYPE_* */
    u16 unix_mode;           /* NYFS_MODE_* 비트 조합 */
    u32 uid;
    u32 gid;
    u32 link_count;          /* 하드링크 개수 */

    u16 ace_count;           /* ace[] 안에서 실제로 쓰이는 개수 (0..NYFS_ACL_INLINE_COUNT) */
    u16 reserved0;
    nyfs_ace_t ace[NYFS_ACL_INLINE_COUNT];
    nyfs_blkno_t acl_overflow_block;  /* 인라인 슬롯이 모자랄 때 추가 ACE 배열이 담긴 블록. 0=없음 */
    u32 acl_overflow_count;           /* 그 블록에 담긴 추가 ACE 개수 */
    u32 reserved1;

    /*
     * 데이터 스트림. 파일이든 디렉터리든 이 필드들만 본다 - 즉 디렉터리도 파일처럼
     * open()/read()/write() 로 내용을 가질 수 있다("cat ." 가 이 스트림을 읽는 것).
     * 자식 목록(아래 dir_hash_root)과는 완전히 독립이다.
     *
     * data_extent_root 는 nyfs_extent_block_header_t 블록을 가리킨다.
     * data_extent_depth == 0 이면 그 블록에 나열된 extent 들이 실제 데이터 블록이고,
     * depth >= 1 이면 그 블록에 나열된 extent 들은 "다음 레벨의 extent 블록"을
     * 가리킨다(전통적인 간접 블록과 같은 개념). 파일이 커질수록 depth 를 늘려서
     * 이론상 파일 크기 상한이 거의 없게(수십 PB 급 볼륨을 목표로 함) 설계한다.
     */
    u64 size;                       /* 데이터 스트림 크기(바이트) */
    nyfs_blkno_t data_extent_root;  /* 0 = 데이터 없음(빈 파일/빈 디렉터리 내용) */
    u8  data_extent_depth;
    u8  reserved2[7];

    /*
     * 자식 목록(디렉터리 전용). type != NYFS_INO_TYPE_DIR 이면 항상 0 이어야 한다.
     * dir_hash_root 는 nyfs_dirhash_header_t 블록(+뒤따르는 버킷 배열/체인)을 가리킨다.
     */
    nyfs_blkno_t dir_hash_root;
    u64 dir_entry_count;

    u64 ctime;              /* 생성 시각(나노초). 시계 없으면 0 */
    u64 mtime;               /* 데이터 스트림 수정 시각 */
    u64 atime;                /* 접근 시각 */

    u64 last_txn_id;          /* 저널 예약 필드. 저널이 없으면 항상 0, 무시된다 */

    u32 checksum;              /* 이 필드를 0으로 둔 채 계산한 inode 전체의 CRC32 */
    u8  reserved3[88];          /* 향후 확장용(확장 속성 포인터, 압축/암호화 플래그 등). 반드시 0 */
} pack nyfs_inode_t;

NX_STATIC_ASSERT(nyfs_inode_size_is_256, sizeof(nyfs_inode_t) == 256);

/* ------------------------------------------------------------------ */
/* 데이터 extent                                                       */
/* ------------------------------------------------------------------ */

typedef struct nyfs_extent {
    u64 start_block;   /* 시작 블록 번호 */
    u64 length;          /* 블록 개수(연속) */
} pack nyfs_extent_t;

NX_STATIC_ASSERT(nyfs_extent_size_is_16, sizeof(nyfs_extent_t) == 16);

typedef struct nyfs_extent_block_header {
    u32 magic;            /* NYFS_EXTENT_MAGIC */
    u32 extent_count;      /* 이 블록에 들어있는 extent 개수 */
    u64 next_block;         /* 같은 레벨의 다음 extent 블록(0=끝) */
    /* 이 헤더 바로 뒤에 nyfs_extent_t extent[extent_count] 가 온다.
     * (가변 길이라 이 구조체 자체에는 배열을 넣지 않는다 - 접근 코드가 헤더 뒤를
     * 직접 인덱싱한다) */
} pack nyfs_extent_block_header_t;

NX_STATIC_ASSERT(nyfs_extent_block_header_size_is_16, sizeof(nyfs_extent_block_header_t) == 16);

/* ------------------------------------------------------------------ */
/* 디렉터리 자식 해시테이블                                              */
/* ------------------------------------------------------------------ */

/* inode.dir_hash_root 가 가리키는 첫 블록의 헤더. 이 헤더 바로 뒤에
 * nyfs_blkno_t bucket[bucket_count] 버킷 배열이 온다. 각 버킷은 같은 이름
 * 해시값(name_hash % bucket_count)을 가진 nyfs_dirent_t 들의 연결 리스트 중
 * 첫 항목이 담긴 블록을 가리킨다(0=빈 버킷). 이름 하나를 찾는 비용은
 * "버킷 하나 찾기(O(1)) + 그 버킷 체인 길이(보통 매우 짧음)"이다. */
typedef struct nyfs_dirhash_header {
    u32 magic;            /* NYFS_DIRHASH_MAGIC */
    u32 bucket_count;      /* 2의 거듭제곱 */
    u64 entry_count;        /* 현재 자식 개수(캐시값. inode.dir_entry_count 와 항상 같아야 한다) */
    u64 free_offset;         /* 이 hash table 영역 안에서 다음 dirent 를 쓸 빈 공간의 오프셋
                               * (단순 bump allocator - 삭제로 생긴 구멍 재사용은 이후 버전에서 다룬다) */
    u32 checksum;
    u32 reserved0;
} pack nyfs_dirhash_header_t;

NX_STATIC_ASSERT(nyfs_dirhash_header_size_is_32, sizeof(nyfs_dirhash_header_t) == 32);

#define NYFS_DIRENT_TYPE_UNKNOWN  0U
#define NYFS_DIRENT_TYPE_FILE     1U
#define NYFS_DIRENT_TYPE_DIR      2U
#define NYFS_DIRENT_TYPE_SYMLINK  3U
#define NYFS_DIRENT_TYPE_DEVICE   4U

/* NYFS_DIRENT_TYPE_* 값은 일부러 NYFS_INO_TYPE_* 와 같은 숫자를 쓴다(번역 불필요). */
NX_STATIC_ASSERT(dirent_type_matches_ino_type_file, NYFS_DIRENT_TYPE_FILE == NYFS_INO_TYPE_FILE);
NX_STATIC_ASSERT(dirent_type_matches_ino_type_dir, NYFS_DIRENT_TYPE_DIR == NYFS_INO_TYPE_DIR);

typedef struct nyfs_dirent {
    u64 ino;
    u64 next_offset;   /* 같은 버킷 체인의 다음 dirent 오프셋. 0=체인 끝
                         * (오프셋 0은 이 hash table 영역의 헤더 자신이 차지하므로
                         * "없음" 표시로 안전하게 재사용할 수 있다) */
    u32 name_hash;       /* 이름의 해시값 - 문자열 비교 전에 먼저 걸러낸다 */
    u16 name_len;          /* 이름 길이(바이트, UTF-8, NUL 미포함) */
    u8  file_type;           /* NYFS_DIRENT_TYPE_* - readdir 이 inode 를 다시 읽지 않고
                               * 타입을 알 수 있게 캐시해 둔다 */
    u8  reserved0;
    /* 이 구조체 바로 뒤에 name_len 바이트의 UTF-8 이름이 오고, 8바이트 경계까지 0으로
     * 패딩된다(다음 dirent 가 항상 8바이트 정렬 오프셋에서 시작하게 하기 위함) */
} pack nyfs_dirent_t;

NX_STATIC_ASSERT(nyfs_dirent_size_is_24, sizeof(nyfs_dirent_t) == 24);

/* 디렉터리 이름 한 구성요소(path segment)의 최대 길이. VFS_NAME_MAX(255)와 맞춘다. */
#define NYFS_NAME_MAX  255U

#endif /* NYFS_DISK_H */
