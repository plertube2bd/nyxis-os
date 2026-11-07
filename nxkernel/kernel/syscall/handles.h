/*
 * handles.h - 프로세스별 핸들 테이블 (capability 방식)
 *
 * 사용자 프로그램은 커널 객체를 포인터/전역 번호가 아니라 "이 프로세스의 핸들 테이블에서만 유효한 핸들"
 * 로 다룬다. 모든 핸들은 권한(NX_RIGHT_*)을 가지며, 시스템 콜은 필요한 권한이 있는지 매번 확인한다.
 *
 * 보안 성질:
 *  - 핸들 값에 세대(generation)가 들어 있어서 닫힌 슬롯이 재사용되어도 오래된 핸들 값은 거부된다.
 *  - 다른 프로세스의 핸들 테이블에는 접근할 방법이 없다 (항상 current_process 의 테이블만 본다).
 *  - 핸들 수는 NX_MAX_HANDLES 로 제한된다 (자원 고갈 방지). 프로세스가 끝나면 모두 닫힌다.
 */
#ifndef KERNEL_SYSCALL_HANDLES_H
#define KERNEL_SYSCALL_HANDLES_H

#include "nyxis.h"
#include "nyx_abi.h"
#include "drivers/filesystem/vfs.h"

#define NX_HANDLE_FREE     0U
#define NX_HANDLE_CONSOLE  1U   /* stdin/stdout/stderr */
#define NX_HANDLE_FILE     2U   /* VFS 파일 */
#define NX_HANDLE_PROCESS  3U   /* 자식 프로세스 (NxProcessCreate). 프로세스 참조 1 개를 가진다 */
#define NX_HANDLE_TOKEN    4U   /* 신원 토큰 (NxAuthenticate). 일회용, 복제 불가 */
#define NX_HANDLE_CHANNEL  5U   /* svc:/ 채널 (수신 또는 송신) */
#define NX_HANDLE_DEVICE   6U   /* dev:/ 장치. 복제 불가 (소유권) */

struct process;
struct auth_identity;
struct channel;
struct nx_device;

#define NX_HANDLE_INVALID  0xFFFFFFFFFFFFFFFFUL

typedef struct nx_handle {
    u32 type;               /* NX_HANDLE_* */
    u32 rights;             /* NX_RIGHT_* 조합 */
    u32 generation;         /* 슬롯 재사용 횟수 */
    u32 console_fd;         /* 콘솔 핸들일 때: 0=in 1=out 2=err */
    handle_t file;          /* 파일 핸들일 때 VFS 핸들 (사본) */
    struct process *proc;   /* 프로세스 핸들일 때 대상 (이 핸들이 참조 1 개를 소유: 닫을 때 process_release) */
    struct auth_identity *token;   /* 토큰 핸들: 커널 힙의 신원 (닫으면 지우고 해제) */
    struct channel *chan;          /* 채널 핸들: 채널 참조 1 개 */
    bool chan_receiver;            /* true = 수신(만든 쪽), false = 송신 */
    struct nx_device *dev;         /* 장치 핸들 */
    bool dev_write;                /* 쓰기로 열었는지 (블록 장치의 독점 여부) */
    u64  dev_offset;               /* 블록 장치 오프셋 */
} nx_handle_t;

typedef struct nx_handle_table {
    nx_handle_t slots[NX_MAX_HANDLES];
} nx_handle_table_t;

/* 핸들 테이블 초기화 + stdin(읽기)/stdout/stderr(쓰기) 콘솔 핸들 생성 */
void nx_handles_init(nx_handle_table_t *table);

/* 열려 있는 모든 핸들을 닫는다 (프로세스 종료 시) */
void nx_handles_close_all(nx_handle_table_t *table);

/* 빈 슬롯을 찾아 핸들을 만들고 그 값을 반환한다. 슬롯이 없으면 NoutOfMemory. */
Nstatus nx_handle_alloc(nx_handle_table_t *table, u32 type, u32 rights, u64 *out_handle,
                        nx_handle_t **out_slot);

/*
 * 핸들 값을 검증하고 슬롯을 돌려준다.
 * 형식/세대가 맞지 않거나 닫힌 핸들이면 NinvalidArg, 필요한 권한이 없으면 Npermission.
 */
Nstatus nx_handle_lookup(nx_handle_table_t *table, u64 handle, u32 required_rights,
                         nx_handle_t **out_slot);

/* 핸들을 닫는다 (파일이면 VFS 핸들도 닫고, 프로세스면 그 참조를 놓는다). */
Nstatus nx_handle_close(nx_handle_table_t *table, u64 handle);

/* 방금 alloc 했지만 아직 쓰지 않은 슬롯을 되돌린다 (VFS 핸들은 닫지 않는다). 세대는 증가시켜 값을 무효화 */
void nx_handle_discard(nx_handle_t *slot);

/* 열려 있는 핸들 수 */
u32 nx_handles_count(const nx_handle_table_t *table);

#endif /* KERNEL_SYSCALL_HANDLES_H */
