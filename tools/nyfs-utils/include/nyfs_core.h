/*
 * nyfs_core.h - NyFS 온디스크 로직을 리눅스 호스트에서 재사용하기 위한 코어
 * 라이브러리 공개 API.
 *
 * 이 라이브러리는 nyxis-os 커널의 drivers/filesystem/nyfs/nyfs.c 를 "디스크에
 * 어떤 바이트를 어떻게 쓰고 읽는지" 레벨에서 그대로 이식한 것이다. 커널
 * 쪽과 다른 것은 오직 I/O 계층뿐이다:
 *   - 커널: diskno + nyfs_disk_read_fn/write_fn 콜백(ATA/AHCI/램디스크)
 *   - 여기: 열린 파일 서술자(fd) 하나에 대한 pread(2)/pwrite(2)
 * 그 위의 비트맵 할당자, inode 테이블, extent 체인, 디렉터리 해시, 권한
 * 검사(UNIX 모드 + NTFS 스타일 ACE) 로직은 바이트 단위로 동일하다 - 그래야
 * 이 툴로 만들거나 손댄 볼륨을 커널이 그대로 마운트할 수 있다.
 *
 * 범위(커널과 동일하게 맞춤 - 2026-09-25 사용자와 합의):
 *   - rename 없음
 *   - ACL 상속 없음(생성 시 항상 UNIX 모드만 받고, ACE 는 빈 채로 시작)
 *   - extent depth 는 0(직접 리스트)만 지원 - data_extent_depth 는 항상 0으로 쓴다
 *   - 저널 없음(journal_version == 0 로 항상 포맷)
 *   - "." / ".." 디렉터리 엔트리 없음(커널 readdir 과 동일하게 실제 자식만 나열)
 * 위 범위를 넘어서는 온디스크 변경은 절대 이 라이브러리 안에서 혼자
 * 시도하지 않는다 - 하게 되면 커널 nyfs.c 와 조용히 포맷이 갈라진다.
 *
 * chmod/chown/utime(setattr) 은 커널에 아직 해당 시스템 콜이 없지만, 여기서
 * 다루는 필드(unix_mode/uid/gid/mtime/atime/ctime) 는 이미 nyfs_disk.h 의
 * inode 구조체에 정의되어 있고 커널도 그대로 읽는 필드라 온디스크 레이아웃을
 * 전혀 바꾸지 않는다 - 그래서 위 "범위 제한"과 별개로 포함시켰다.
 */
#ifndef NYFS_CORE_H
#define NYFS_CORE_H

#include "nyfs_disk.h"

/* ------------------------------------------------------------------ */
/* 볼륨 핸들                                                            */
/* ------------------------------------------------------------------ */

typedef struct nyfs_volume {
    int fd;                 /* 이미지 파일 또는 블록 장치의 열린 fd */
    nbool writable;
    nyfs_superblock_t sb;    /* 메모리 캐시. 바뀔 때마다 블록 0 에 다시 쓴다 */
    u32 block_size;           /* 1 << sb.block_size_shift */
} nyfs_volume_t;

typedef struct {
    u32 uid;
    u32 gid;
} nyfs_cred_t;

/* mkfs 파라미터. total_blocks/label 이외는 0 을 넘기면 커널 nyfs_format() 과
 * 동일한 기본값 계산 규칙을 그대로 따른다(inode_count==0 이면
 * total_blocks/8, 최소 16). */
typedef struct {
    u8          block_size_shift;   /* 0 이면 NYFS_BLOCK_SIZE_SHIFT_MIN(512B) 사용 안 함 -
                                      * 호출자가 반드시 유효 범위 값을 채워야 한다 */
    u64         total_blocks;
    u64         inode_count;        /* 0 = 자동 계산 */
    const char *label;               /* nNULL 이면 라벨 없음(전부 0) */
} nyfs_mkfs_params_t;

/* readdir 콜백 하나가 볼 항목 하나. 이름은 NUL 종결 UTF-8. */
typedef struct {
    char name[NYFS_NAME_MAX + 1U];
    nyfs_ino_t ino;
    u32 type;   /* NYFS_DIRENT_TYPE_* */
} nyfs_dirent_view_t;

/* ------------------------------------------------------------------ */
/* mkfs                                                                */
/* ------------------------------------------------------------------ */

/* fd 가 가리키는 파일/블록장치 전체를 NyFS 로 포맷한다. 정규 파일이면
 * params->total_blocks*(1<<block_size_shift) 바이트로 ftruncate 까지
 * 이 함수가 해준다(호출자는 open() 만 해서 넘기면 된다). */
nyfs_status_t nyfs_format(int fd, const nyfs_mkfs_params_t *params);

/* ------------------------------------------------------------------ */
/* 마운트(=볼륨 열기) / 언마운트                                         */
/* ------------------------------------------------------------------ */

/* 슈퍼블록을 읽어 매직/버전/크기/체크섬을 검증하고 vol 을 채운다. */
nyfs_status_t nyfs_volume_open(nyfs_volume_t *vol, int fd, nbool writable);
void nyfs_volume_close(nyfs_volume_t *vol);

/* ------------------------------------------------------------------ */
/* inode 단위 연산                                                      */
/* ------------------------------------------------------------------ */

nyfs_status_t nyfs_read_inode(nyfs_volume_t *vol, nyfs_ino_t ino, nyfs_inode_t *out);
nyfs_status_t nyfs_write_inode(nyfs_volume_t *vol, nyfs_ino_t ino, nyfs_inode_t *in);

/* 경로("/" 로 시작하거나 안 해도 루트 기준 상대로 취급) 를 따라가며 각
 * 디렉터리에서 EXECUTE(순회) 권한을 검사한다. */
nyfs_status_t nyfs_path_lookup(nyfs_volume_t *vol, const char *path,
                                const nyfs_cred_t *cred, nyfs_ino_t *out_ino);

/* 경로의 마지막 컴포넌트를 떼어 부모 디렉터리 inode 번호와 이름을 돌려준다
 * (create/mkdir/unlink 가 공통으로 쓴다). 루트("/") 자체는 부모가 없으므로
 * NinvalidArg. */
nyfs_status_t nyfs_path_lookup_parent(nyfs_volume_t *vol, const char *path,
                                       const nyfs_cred_t *cred,
                                       nyfs_ino_t *out_parent_ino,
                                       char *out_name, usize name_max);

nyfs_status_t nyfs_op_create(nyfs_volume_t *vol, nyfs_ino_t parent_ino, const char *name,
                              u16 unix_mode, const nyfs_cred_t *cred, nyfs_ino_t *out_ino);
nyfs_status_t nyfs_op_mkdir(nyfs_volume_t *vol, nyfs_ino_t parent_ino, const char *name,
                             u16 unix_mode, const nyfs_cred_t *cred, nyfs_ino_t *out_ino);

/* 파일과 빈 디렉터리 둘 다 지운다(커널 nyfs_unlink 와 동일 규칙: 디렉터리인데
 * dir_entry_count > 0 이면 Nbusy). expect_dir 로 FUSE 의 unlink/rmdir 구분을
 * 강제할 수 있다(nfalse 면 디렉터리 대상 거부, ntrue 면 비-디렉터리 대상 거부;
 * 어느 쪽이든 상관없으면 값 대신 검사를 생략하려면 nyfs_op_remove_any 사용). */
nyfs_status_t nyfs_op_remove(nyfs_volume_t *vol, nyfs_ino_t parent_ino, const char *name,
                              const nyfs_cred_t *cred, nbool expect_dir);

nyfs_status_t nyfs_op_read(nyfs_volume_t *vol, nyfs_ino_t ino, const nyfs_cred_t *cred,
                            u64 offset, void *buf, usize size, usize *out_read);
nyfs_status_t nyfs_op_write(nyfs_volume_t *vol, nyfs_ino_t ino, const nyfs_cred_t *cred,
                             u64 offset, const void *buf, usize size, usize *out_written);

/* index 번째(0-based) 자식을 돌려준다. 커널 nyfs_readdir 과 동일하게
 * "." / ".." 는 포함하지 않는다 - FUSE 글루 쪽에서 필요하면 그 둘만 앞에
 * 따로 얹는다. index 가 범위를 벗어나면 NnotFound. */
nyfs_status_t nyfs_op_readdir_at(nyfs_volume_t *vol, nyfs_ino_t dir_ino,
                                  const nyfs_cred_t *cred, u64 index,
                                  nyfs_dirent_view_t *out);

/* chmod/chown/utimens 대응. 커널에 아직 없는 시스템 콜이지만 위 주석대로
 * 온디스크 포맷은 그대로다. mtime_ns/atime_ns 가 각각 NYFS_TIME_KEEP 이면
 * 그 필드는 바꾸지 않는다. */
#define NYFS_TIME_KEEP  ((u64)-1)   /* u64 는 부호 없는 타입이므로 -1 은 전 비트 1 로 정의 동작(C89) */

nyfs_status_t nyfs_op_setattr(nyfs_volume_t *vol, nyfs_ino_t ino, const nyfs_cred_t *cred,
                               int set_mode, u16 unix_mode,
                               int set_owner, u32 uid, u32 gid,
                               u64 mtime_ns, u64 atime_ns);

/* size 로 자른다(0 이상만 지원 - 파일을 늘리는 truncate 는 write 로 구멍을
 * 메우는 것과 달리 extent 회수까지 다뤄야 해서 지금은 "줄이기"만 지원한다.
 * 늘리는 요청은 size 만 키우고 실제 블록은 read 시 0 으로 채워 보여준다 -
 * 이는 커널의 "읽기는 extent_resolve 실패 시 끝으로 취급" 동작과는 다르므로
 * 늘리는 truncate 는 Nunsupported 로 명시적으로 거부한다). */
nyfs_status_t nyfs_op_truncate(nyfs_volume_t *vol, nyfs_ino_t ino, const nyfs_cred_t *cred, u64 new_size);

#endif /* NYFS_CORE_H */
