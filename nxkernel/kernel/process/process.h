/*
 * process.h - 프로세스/커널 스레드 관리 (협력형 스케줄링)
 *
 * [기존 코드의 문제]
 *  - process_create() 가 process_list 포인터 + sizeof(process_t) * pid 라는 "임의의
 *    메모리 주소"에 프로세스 구조체를 썼다. 그 주소는 아무도 할당한 적이 없으므로
 *    커널 데이터/코드를 마음대로 덮어쓰는 메모리 손상이었다.
 *  - process_switch() 는 인라인 asm 을 문장별로 나눠서 레지스터를 하나씩 읽고/쓰는
 *    방식이었다. 컴파일러는 asm 문장 사이에 레지스터를 재사용하므로 저장/복원한 값이
 *    의미가 없고, rsp 를 바꾼 뒤 C 코드가 계속 실행되어 스택이 붕괴한다.
 *    -> 어셈블리(switch.s)로 작성한 callee-saved 레지스터 + rsp 교체 방식으로 교체.
 *  - 프로세스 구조체를 정적 테이블에서 할당하도록 변경했다. (동적 메모리 할당자 부재)
 *
 * 현재 지원: 커널 스레드 생성/종료/양보(협력형 라운드 로빈).
 * 미지원(추후): 타이머 기반 선점, ring3 프로세스 로딩, 프로세스별 주소 공간.
 */
#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include "nyxis.h"
#include "kernel/process/schedule.h"   /* schedule(): 다음 READY 프로세스로 양보 */
#include "kernel/syscall/handles.h"      /* 프로세스별 핸들 테이블 */

#define PROCESS_MAX          16U
#define PROCESS_KSTACK_SIZE  16384U

/* Process states */
typedef enum {
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_TERMINATED
} process_state_t;

/* 스레드 진입 함수: 인자로 process_create 의 stack 값이 전달된다 */
typedef void (*process_entry_t)(void *arg);

typedef struct process {
    u32   pid;
    u32   state;

    void *stack;          /* 인자로 전달되는 값(예: 유저 스택). 커널 스레드는 NULL 가능 */
    process_entry_t entry_point;

    void *cr3;            /* 0 이면 주소 공간 전환 없음 */

    u64   kernel_rsp;     /* 컨텍스트 스위치 시 저장된 커널 rsp */
    void *kernel_stack;   /* 커널 스택의 최하위 주소 (부트 스레드는 NULL) */

    u64   wake_tick;      /* BLOCKED(sleep) 상태일 때 깨어날 tick. 0 이면 타이머 대기 아님 */
    i32   exit_code;      /* 종료 코드 (NxProcessExit 인자, 예외로 죽으면 -1) */

    nx_handle_table_t handles;   /* 이 프로세스의 핸들 테이블 (stdin/stdout/stderr + 열린 파일) */

    bool  in_use;
    struct process *next;
} process_t;

/* Global process state */
extern process_t *process_list;
extern process_t *current_process;

/* 부트 스레드를 pid 0(idle) 프로세스로 등록한다. */
Nstatus process_init(void);

/* 새 커널 스레드를 만든다. stack 값이 entry_point 의 인자로 전달된다. */
Nstatus process_create(process_entry_t entry_point, void *stack);

/* proc 으로 문맥 전환한다. (proc 은 READY 상태여야 함) */
Nstatus process_switch(process_t *proc);

/* pid 프로세스를 종료한다. 자기 자신이면 반환하지 않는다. pid 0 은 종료할 수 없다. */
Nstatus process_terminate(u32 pid);

/* 현재 프로세스 종료 (반환하지 않음). 스레드 함수가 return 하면 자동 호출된다. */
void process_exit(void) __attribute__((noreturn));

/* 종료 코드를 남기고 현재 프로세스를 종료한다 (반환하지 않음). 핸들은 모두 닫힌다. */
void process_exit_with_code(i32 code) __attribute__((noreturn));

/* 마지막으로 종료된 프로세스의 종료 코드 (자체 점검/디버그용) */
i32 process_last_exit_code(void);

/*
 * 현재 프로세스를 ticks 만큼 재운다 (BLOCKED -> 시간이 되면 스케줄러가 READY 로 되돌림).
 * idle(pid 0)은 재울 수 없다 (Npermission). 다른 실행 대상이 없어 못 잤으면 Ninterrupted.
 */
Nstatus process_sleep_ticks(u64 ticks);

/* 살아 있는(종료되지 않은) 프로세스 수 */
u32 process_count(void);

/* asm 에서 호출: 스레드 함수가 반환했을 때의 종료 처리 */
void process_thread_exit(void) __attribute__((noreturn));

/* switch.s */
void cpu_switch_context(u64 *save_rsp, u64 new_rsp);
void process_entry_trampoline(void);

#endif /* KERNEL_PROCESS_H */
