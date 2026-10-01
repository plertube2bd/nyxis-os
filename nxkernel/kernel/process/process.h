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
 * [프로세스별 주소 공간]
 *  - 커널 스레드(process_create)는 전용 주소 공간이 없다 (addrspace.pml4 == NULL). 이런
 *    스레드는 항상 "커널 전용" 주소 공간(paging_kernel_cr3())을 공유해서 실행된다 — 사용자
 *    영역이 아예 없는, HHDM/커널 이미지만 있는 주소 공간이다.
 *  - 사용자 프로세스(process_create_user)는 paging_addrspace_create() 로 만든 독립된
 *    PML4 를 가진다 (addrspace.pml4 != NULL). process_switch() 가 이 필드를 보고 CR3 를
 *    항상 "현재 실행하는 프로세스에 맞게" 바꾼다 — 그래야 시스템 콜이 검사하는 주소 공간
 *    (current_process->addrspace)과 CPU 가 실제로 쓰는 CR3 가 항상 일치한다.
 *  - 프로세스가 완전히 정리(process_reap)될 때 전용 주소 공간이면 paging_addrspace_destroy()
 *    로 회수한다 (모든 PT/PD/PDPT/PML4 및 그 안의 데이터 프레임이 pfa.h 로 돌아간다).
 *
 * 현재 지원: 커널 스레드/사용자 프로세스 생성·종료·양보(협력형 라운드 로빈), 프로세스별 주소 공간.
 * 미지원(추후): 타이머 기반 선점, 프로세스 간 fork/exec, 자식 프로세스 wait.
 */
#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include "nyxis.h"
#include "kernel/process/schedule.h"   /* schedule(): 다음 READY 프로세스로 양보 */
#include "kernel/syscall/handles.h"      /* 프로세스별 핸들 테이블 */
#include "kernel/paging/paging.h"        /* addr_space_t */

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

    /* 파일 권한 검사(UNIX 모드/ACL)가 참조하는 호출자 신원.
     * 아직 ring3 프로세스/로그인 개념이 없어 전부 0(root)으로 시작하고, 모든
     * process_create() 경로가 memset(0) 을 거치므로 별도 초기화 없이 안전하다.
     * 실제 사용자 인증이 생기면 그 경로에서 이 값을 채워 넣으면 된다. */
    u32   uid;
    u32   gid;

    void *stack;          /* 인자로 전달되는 값(예: 유저 스택). 커널 스레드는 NULL 가능 */
    process_entry_t entry_point;

    addr_space_t addrspace;   /* pml4 == NULL 이면 전용 주소 공간이 없는 커널 스레드 */
    u64   user_entry;         /* 사용자 프로세스: ring3 진입점 가상 주소 (ELF e_entry) */
    u64   user_stack_top;     /* 사용자 프로세스: ring3 진입 시 RSP (스택 최상단) */
    u64   heap_start;         /* 사용자 프로세스: 힙(brk) 시작 = ELF 로드 이미지 끝 (페이지 정렬) */
    u64   heap_brk;           /* 사용자 프로세스: 현재 brk (힙 끝). heap_start 로 초기화됨 */

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

/*
 * 독립된 주소 공간을 가진 새 사용자(ring3) 프로세스를 만든다.
 *   as             : paging_addrspace_create() 로 이미 만들어진, 사용자 영역(ELF 세그먼트 +
 *                     스택)까지 전부 매핑이 끝난 주소 공간. 실패하면 이 함수가 대신 회수하지
 *                     않으므로 호출자가 paging_addrspace_destroy() 해야 한다.
 *   entry_vaddr    : ring3 에서 실행을 시작할 가상 주소 (ELF e_entry)
 *   stack_top_vaddr: ring3 진입 시 RSP (스택 최상단, 이미 as 안에 매핑되어 있어야 함)
 *   heap_start_vaddr : 힙(brk) 시작 가상 주소 (보통 ELF 로드 이미지 끝, 페이지 정렬)
 *   out_pid        : 성공하면 새 프로세스의 pid
 */
Nstatus process_create_user(
    addr_space_t as,
    u64 entry_vaddr,
    u64 stack_top_vaddr,
    u64 heap_start_vaddr,
    u32 *out_pid
);

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

/*
 * 이미 종료된(TERMINATED) 프로세스들의 자원(전용 주소 공간 등)을 지금 즉시 회수한다.
 * 평소에는 다음 프로세스 생성/종료 때 자동으로 수행되지만, 자원 회수를 정확히 확인해야 하는
 * 자체 점검이나 메모리 압박 시 명시적으로 부를 수 있다.
 */
void process_reap_terminated(void);

/*
 * 종료된(TERMINATED) 프로세스의 종료 코드를 조회한다. 아직 회수(reap)되기 전이어야 한다.
 * 살아 있으면 Nbusy, 그런 pid 가 없으면(이미 회수되었거나 잘못된 pid) NnotFound.
 * (이후 NxProcessWait 의 기반이 된다)
 */
Nstatus process_get_exit_code(u32 pid, i32 *out_code);

/* asm 에서 호출: 스레드 함수가 반환했을 때의 종료 처리 */
void process_thread_exit(void) __attribute__((noreturn));

/* switch.s */
void cpu_switch_context(u64 *save_rsp, u64 new_rsp);
void process_entry_trampoline(void);

#endif /* KERNEL_PROCESS_H */
