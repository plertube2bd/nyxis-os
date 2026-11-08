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
 *
 * 공유 객체: 파일 핸들은 "열린 파일"(nx_ofile_t, 오프셋을 가진 VFS 핸들 + 참조 수)을 가리킨다.
 * NxDuplicateHandleEx(NX_DUP_SHARE)/NxProcessFork/상속으로 생긴 핸들들은 같은 열린 파일을 공유한다
 * (POSIX 의 open file description). 파이프/채널/프로세스도 참조 수로 공유된다.
 *
 * 물려줄 수 있는 종류 (fork, NX_SPAWN_INHERIT): 콘솔, 파일, 파이프, 채널 송신 끝, info 스냅샷(사본).
 * 물려주지 않는 종류: 프로세스(자식 관계는 물려받지 않는다), 토큰(일회용 비밀), 장치(독점 소유),
 * 채널 수신 끝(받는 쪽은 하나).
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
#define NX_HANDLE_INFO     7U   /* info:/ 가상 파일: 열 때 만든 스냅샷 (읽기 전용) */
#define NX_HANDLE_PIPE     8U   /* 파이프의 한쪽 끝 (kernel/ipc/pipe.h) */

struct process;
struct auth_identity;
struct channel;
struct nx_device;
struct nx_pipe;

/* 열린 파일 (공유 가능). refs 가 0 이 되면 VFS 핸들을 닫고 해제한다 */
typedef struct nx_ofile {
    handle_t file;
    u32 refs;
    bool append;            /* NX_O_APPEND: 쓰기마다 먼저 파일 끝으로 */
} nx_ofile_t;

#define NX_HANDLE_INVALID  0xFFFFFFFFFFFFFFFFUL

typedef struct nx_handle {
    u32 type;               /* NX_HANDLE_* */
    u32 rights;             /* NX_RIGHT_* 조합 */
    u32 generation;         /* 슬롯 재사용 횟수 */
    u32 console_fd;         /* 콘솔 핸들일 때: 0=in 1=out 2=err */
    u32 hflags;             /* NX_HFLAG_* (NX_HFLAG_CLOEXEC) */
    nx_ofile_t *of;         /* 파일 핸들일 때 열린 파일 (참조 1 개) */
    struct nx_pipe *pipe;   /* 파이프 핸들일 때 (그 끝의 참조 1 개) */
    bool pipe_write;        /* true = 쓰기 끝 */
    struct process *proc;   /* 프로세스 핸들일 때 대상 (이 핸들이 참조 1 개를 소유: 닫을 때 process_release) */
    struct auth_identity *token;   /* 토큰 핸들: 커널 힙의 신원 (닫으면 지우고 해제) */
    struct channel *chan;          /* 채널 핸들: 채널 참조 1 개 */
    bool chan_receiver;            /* true = 수신(만든 쪽), false = 송신 */
    struct nx_device *dev;         /* 장치 핸들 */
    bool dev_write;                /* 쓰기로 열었는지 (블록 장치의 독점 여부) */
    u64  dev_offset;               /* 블록 장치 오프셋 */
    char *info_buf;                /* info 핸들: kmalloc 한 스냅샷 (디렉터리면 NULL). 닫으면 해제 */
    u64  info_len;
    u64  info_offset;
    bool info_dir;
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

/* VFS 핸들 f 로 새 열린 파일을 만든다 (refs = 1). 실패하면 NULL (f 는 닫지 않는다) */
nx_ofile_t *nx_ofile_new(const handle_t *f);

/*
 * src 가 가리키는 객체를 공유하는 사본을 dst 에 채운다 (참조 수를 늘린다). dst 의 type/rights/hflags 는
 * 호출자가 정한다 (여기서는 객체 포인터만). share 가 false 이면 파일은 새로 열어 오프셋만 복사한다.
 * 물려줄 수 없는 종류면 Nunsupported.
 */
Nstatus nx_handle_share(const nx_handle_t *src, nx_handle_t *dst, bool share);

/*
 * src 테이블을 dst 로 물려준다 (dst 의 기존 핸들은 먼저 모두 닫는다).
 *   fork  (keep_generation = true,  skip_cloexec = false): 슬롯 번호와 세대를 그대로
 *   spawn (keep_generation = false, skip_cloexec = true) : 슬롯 번호는 그대로, 세대는 0
 * 물려줄 수 없는 종류의 슬롯은 dst 에서 닫힌 상태다. 열린 파일 사본을 만들 메모리가 없으면 NoutOfMemory
 * (이미 물려준 핸들은 dst 에 남아 있다: 호출자가 dst 프로세스를 버리면 함께 닫힌다).
 */
Nstatus nx_handles_inherit(nx_handle_table_t *dst, const nx_handle_table_t *src,
                           bool keep_generation, bool skip_cloexec);

/* NxProcessExec: NX_HFLAG_CLOEXEC 핸들을 닫고, 모든 슬롯의 세대를 0 으로 되돌린다 */
void nx_handles_exec(nx_handle_table_t *table);

/* 슬롯 번호 index 의 핸들 값 (열려 있지 않으면 NX_HANDLE_INVALID) */
u64 nx_handle_value(const nx_handle_table_t *table, u32 index);

#endif /* KERNEL_SYSCALL_HANDLES_H */
