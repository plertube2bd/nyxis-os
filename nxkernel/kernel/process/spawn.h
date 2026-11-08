/*
 * spawn.h - 실행 파일 경로로부터 새 사용자 프로세스를 만든다 (NxProcessCreate 의 실제 구현)
 */
#ifndef KERNEL_PROCESS_SPAWN_H
#define KERNEL_PROCESS_SPAWN_H

#include "nyxis.h"
#include "kernel/process/process.h"

/*
 * 새 프로세스에 넘길 인자/환경 (커널 메모리 안에 있어야 한다).
 *   strings: argv[0..argc-1] 다음 envp[0..envc-1] 문자열을 각각 NUL 로 끝내 이어 붙인 것.
 *   size   : strings 의 총 바이트 (마지막 NUL 포함). argc+envc 개의 NUL 이 정확히 들어 있어야 한다.
 * 상한: argc+envc <= NX_ARGV_MAX, size <= NX_ARG_MAX (넘으면 Noverflow).
 */
typedef struct spawn_args {
    u32 argc;
    u32 envc;
    usize size;
    const char *strings;
} spawn_args_t;

/*
 * path 의 ELF64 실행 파일을 열어 새 주소 공간에 적재하고, 64KiB 스택(+가드 페이지)을 만들어
 * argv/envp 를 System V 초기 스택 배치로 써 넣고(args 가 NULL 이면 argc=0), READY 상태의
 * 새 프로세스를 등록한다. 성공하면 *out_proc 에 새 프로세스 — 호출자가 참조 1 개를 가지며
 * 다 쓰면 process_release() 해야 한다 (시스템 콜 경로에서는 프로세스 핸들이 그 참조를 가진다).
 *
 * 어느 단계에서 실패하든 이 함수가 만든 모든 것(열린 파일, 주소 공간 전체 = 그 안의 모든
 * 프레임과 페이지 테이블)을 되돌리고 오류를 반환한다.
 */
Nstatus process_spawn(const char *path, const spawn_args_t *args, process_t **out_proc);

/* 실행 파일로 만든 새 프로그램 이미지 (아직 어떤 프로세스에도 붙지 않은 주소 공간) */
typedef struct spawn_image {
    addr_space_t as;
    u64 entry;          /* 진입점 (ELF e_entry) */
    u64 stack;          /* 초기 rsp (argv/envp 를 쓴 System V 스택) */
    u64 image_end;      /* 힙(brk) 시작 */
} spawn_image_t;

/*
 * process_spawn 의 앞부분: path 를 실행 권한으로 열어 새 주소 공간에 적재하고 스택을 만든다.
 * NxProcessExec 도 이것으로 새 이미지를 끝까지 만든 뒤에야 옛 이미지를 버린다.
 * 실패하면 만든 것을 모두 되돌린다. 성공하면 out->as 의 소유권은 호출자에게 있다.
 */
Nstatus spawn_build_image(const char *path, const spawn_args_t *args, spawn_image_t *out);

#endif /* KERNEL_PROCESS_SPAWN_H */
