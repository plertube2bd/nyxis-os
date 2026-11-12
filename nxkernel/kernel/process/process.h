/*
 * process.h - 프로세스/커널 스레드 관리 (라운드 로빈, 사용자 모드 선점 — schedule.h)
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
 * [수명 주기: 자원과 슬롯을 분리한다]
 *  - 종료(TERMINATED)된 프로세스의 "자원"(주소 공간, 핸들)은 CPU 가 그 프로세스에서 벗어나는 즉시
 *    회수한다 (schedule()/생성/종료 경로의 process_reap). 메모리가 다음 프로세스 생성 때까지
 *    묶여 있지 않는다.
 *  - "슬롯"(pid + 종료 코드)은 참조 카운트(refs)로 관리한다. 참조하는 쪽이 있는 동안에는 종료 후에도
 *    남아 종료 코드를 읽을 수 있고, 마지막 참조가 사라지면(process_release) 회수된다.
 *      * 사용자 프로세스(process_create_user)는 만든 쪽이 참조 1 개를 받는다. 시스템 콜 경로에서는 그 참조가
 *        "프로세스 핸들"(NX_HANDLE_PROCESS)이 된다 — 핸들을 닫거나, 핸들을 가진 프로세스가 끝나면 해제된다.
 *        그래서 부모가 먼저 끝나도 자식 슬롯이 새지 않는다 (부모의 핸들이 닫히면서 참조가 풀린다).
 *      * 커널 스레드(process_create)는 참조 0 으로 시작하므로 종료 즉시 슬롯도 회수된다.
 *      * process_wait() 는 대기 큐(waitq.h)로 종료를 기다린다 (폴링하지 않는다).
 *  - pid 는 단조 증가하며, 한 바퀴 돌면 0(idle)과 사용 중인 pid 를 건너뛴다.
 *
 * [커널 스택 가드]
 *  - 각 커널 스택 바로 아래 4KiB 는 매핑하지 않는다 (process_init 이 해제). 커널 스택이 넘치면
 *    옆 프로세스의 커널 스택을 조용히 덮어쓰는 대신 즉시 #PF -> #DF(IST 스택) -> 패닉 메시지가 난다.
 *
 * 현재 지원: 커널 스레드/사용자 프로세스 생성·종료·양보, 사용자 모드 타이머 선점(커널 코드는 비선점),
 *            프로세스별 주소 공간, 참조 카운트 기반 수명(프로세스 핸들), wait/kill, 대기 큐.
 * 미지원(추후): 커널 선점, 스레드, SMP.
 */
#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include "nyxis.h"
#include "nyx_abi.h"
#include "kernel/process/schedule.h"   /* schedule(): 다음 READY 프로세스로 양보 */
#include "kernel/syscall/handles.h"      /* 프로세스별 핸들 테이블 */
#include "kernel/paging/paging.h"        /* addr_space_t */
#include "kernel/process/waitq.h"        /* waitq_t */
#include "interrupt.h"                   /* struct trap_frame */

#define PROCESS_MAX          32U
#define PROCESS_KSTACK_SIZE  16384U
#define NX_USER_NAME_MAX     32U        /* 사용자 이름 최대 길이 (NUL 포함) */
#define PROCESS_KSTACK_GUARD 4096U      /* 스택 아래 매핑하지 않는 가드 페이지 (PAGE_SIZE 와 같아야 함) */

/* Process states */
typedef enum {
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_TERMINATED
} process_state_t;

/* 스레드 진입 함수: 인자로 process_create 의 stack 값이 전달된다 */
typedef void (*process_entry_t)(void *arg);

#define PROCESS_FPU_STATE_SIZE 512   /* FXSAVE 영역 */

typedef struct process {
    u32   pid;
    u32   state;

    /* 신원. 파일 권한 검사(nyfs UNIX 모드/ACL), 이름공간 해석(face:/ = fs:/home/<user_name>/),
     * fs:/·roi:/·dev:/ 접근(uid 0 만)이 참조한다. 새 프로세스는 만든 프로세스의 신원을 그대로
     * 물려받고(첫 프로세스 init 은 커널이 root 로 만든다), 다른 신원은 커널이 인증 후 발급한
     * 일회용 토큰으로만 얻는다 (kernel/security/auth.h). */
    u32   uid;
    u32   gid;
    char  user_name[NX_USER_NAME_MAX];

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

    u32   parent_pid;     /* 생성한 프로세스의 pid (idle/부트 스레드가 만들었으면 0). 정보용 */
    u32   pgid;           /* 프로세스 그룹 (Ctrl-C 가 함께 끝내는 묶음, NxProcessGroup). 만든 프로세스에게서
                           * 물려받는다. 커널이 만든 프로세스는 자기 pid */
    u32   refs;           /* 이 슬롯을 참조하는 수 (프로세스 핸들 등). 0 이고 종료되었으면 슬롯 회수 */
    u32   slice_ticks;    /* 남은 타임슬라이스 (사용자 모드 선점, schedule.h) */
    u16   io_base;        /* ring3 에서 직접 in/out 할 수 있는 포트 범위 (dev:/com1 의 NX_DEVCMD_IOPERM). */
    u16   io_count;       /* 0 이면 없음. 전환할 때마다 TSS I/O 비트맵에 반영된다 */
    u32   umask;          /* NxCreateNode 의 mode 에서 지울 비트 (NxSetUmask). fork/생성 시 물려준다 */

    /* 제어 터미널 (dev:/tty, 0/1/2 콘솔 핸들의 대상). NULL 이면 물리 콘솔. pty 면 종속 끝 참조 1 개를 가진다
     * (tty_retain). 만든 프로세스에게서 물려받고, 새 프로세스의 0 번 핸들이 pty 종속 끝이면 그 pty 가 된다 */
    struct tty *ctty;
    /* 콘솔 세션 (NX_SPAWN_CONSOLE_SESSION 으로 만들어졌다): dev:/console 을 열 수 있다. 물려주지 않는다 */
    bool  console_session;
    /* 보호 프로세스 (init 은 커널이, login 은 init 이 NX_SPAWN_CRITICAL 로): 부모 말고는 root 도 신호/종료를
     * 보낼 수 없다 (signal_protected). 물려주지 않는다 */
    bool  critical;
    /* ntop / NxProcessList 용: 실행 파일 이름 (경로의 마지막 이름에서 ".run" 을 뗀 것. 생성/exec 때 커널이 정한다,
     * argv[0] 과 달리 사용자가 꾸밀 수 없다. fork 는 물려받는다), 이 프로세스가 CPU 를 쓴 타이머 틱 수, 시작 틱 */
    char  name[NX_PROC_NAME_MAX];
    u64   cpu_ticks;
    u64   start_tick;

    /* 실행 계층 (kernel/exec/exec.h) */
    struct trap_frame *uframe;      /* 지금 처리 중인 시스템 콜의 사용자 레지스터 (syscall_handle 이 설정) */
    struct trap_frame fork_frame;   /* fork 로 만든 자식: 처음 사용자 모드로 돌아갈 레지스터 (rax = 0) */

    waitq_t exit_waiters;          /* 이 프로세스의 종료를 기다리는 프로세스들 */
    waitq_entry_t *wait_entries;   /* 지금 매달려 있는 대기 항목들 (여러 큐 가능, NxPoll). 없으면 NULL */
    bool  wait_woken;              /* 마지막 대기에서 누군가 깨웠는지 (false 면 시간 초과) */

    nx_handle_table_t handles;   /* 이 프로세스의 핸들 테이블 (stdin/stdout/stderr + 열린 파일) */

    /* 신호 (kernel/process/signal.h). 집합은 비트 (sig - 1) */
    struct {
        u64 handler;             /* NX_SIG_DFL / NX_SIG_IGN / 사용자 주소 */
        u64 restorer;
        u32 mask;
        u32 flags;
    } sig_action[NX_SIG_MAX];
    u32   sig_pending;           /* 와서 아직 처리하지 않은 신호 */
    u32   sig_mask;              /* 막은 신호 */
    u32   sig_suspend_mask;      /* NxSignalSuspend 직전의 막은 집합 (sig_suspended 일 때) */
    u32   sig_sender[NX_SIG_MAX];  /* 대기 중인 신호를 보낸 pid (nx_siginfo) */
    bool  sig_suspended;
    bool  sig_woken;             /* 잠든 동안 신호가 깨웠다 (waitq_sleep 이 Ninterrupted 를 돌려준다) */
    bool  sig_hold;              /* 신호로 깨우지 않는 잠 (로그인 실패 지연) */
    bool  sig_iret;              /* NxSignalReturn: sysret 대신 iretq 로 돌아간다 (rcx/r11 까지 되돌리려고) */

    bool  in_use;
    struct process *next;

    /* 사용자 FPU/x87/SSE 상태 (FXSAVE64 형식). process_switch 가 전환할 때마다 저장/복원한다
     * (즉시 방식: CR0.TS 지연 전환을 쓰지 않는다). 커널 코드는 -mgeneral-regs-only 로 빌드되어
     * 이 레지스터들을 건드리지 않으므로, 인터럽트/시스템 콜 경로에서는 따로 저장할 필요가 없다.
     * 새 프로세스는 FNINIT 직후와 같은 상태(FCW 0x037F, MXCSR 0x1F80)로 시작한다 — 다른
     * 프로세스의 레지스터 값이 새어 나가지 않는다. */
    u8    fpu_state[PROCESS_FPU_STATE_SIZE] __attribute__((aligned(16)));
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
 *   out_proc       : 성공하면 새 프로세스. 호출자는 참조 1 개를 가지며 다 쓰면 process_release() 해야 한다
 */
Nstatus process_create_user(
    addr_space_t as,
    u64 entry_vaddr,
    u64 stack_top_vaddr,
    u64 heap_start_vaddr,
    process_t **out_proc
);

/* proc 으로 문맥 전환한다. (proc 은 READY 상태여야 함) */
Nstatus process_switch(process_t *proc);

/* pid 프로세스를 종료한다 (종료 코드 NX_EXIT_KILLED). 자기 자신이면 반환하지 않는다. pid 0 은 종료할 수 없다. */
Nstatus process_terminate(u32 pid);

/* proc 를 종료 코드 code 로 종료한다 (이미 끝났으면 아무것도 하지 않고 성공). 자기 자신이면 반환하지 않는다. */
Nstatus process_kill(process_t *proc, i32 code);

/* path 의 마지막 이름(".run" 을 뗀 것)을 proc->name 에 */
void process_set_name(process_t *proc, const char *path);

/* 살아 있는 프로세스를 out 에 최대 max 개 채운다. 살아 있는 프로세스 수를 돌려준다 (max 보다 클 수 있다) */
u32 process_list_snapshot(struct nx_procent *out, u32 max);

/* 제어 터미널을 바꾼다 (참조를 옮긴다: t 를 retain 하고 이전 것을 release) */
void process_set_ctty(process_t *proc, struct tty *t);

/*
 * 새 프로세스(spawn/fork)나 exec 한 프로세스의 제어 터미널을 정한다: 0 번 핸들이 pty 종속 끝이면 그 pty,
 * 아니면 inherited (부모의 것, exec 이면 자기 것). fresh_console: 핸들을 물려받지 않고 새로 만든 0/1/2 콘솔
 * 핸들이면 그것들도 그 제어 터미널을 가리키게 한다 (0 번 핸들은 보지 않는다).
 */
void process_adopt_ctty(process_t *proc, struct tty *inherited, bool fresh_console);

/* 제어 터미널이 t 인 살아 있는 프로세스 모두에게 SIGHUP (pty 끊김). 인터럽트를 막은 상태에서 */
void process_hangup_ctty(struct tty *t);

/* 그룹 pgid 의 살아 있는 프로세스가 모두 uid 이면 true (비어 있어도 true) */
bool process_group_owned_by(u32 pgid, u32 uid);

/* 참조 카운트. 참조를 가진 동안 proc 포인터는 유효하다(슬롯이 재사용되지 않는다). */
void process_retain(process_t *proc);
void process_release(process_t *proc);

/*
 * proc 가 종료될 때까지 기다려 종료 코드를 돌려준다. 호출자는 proc 의 참조를 가지고 있어야 한다.
 *   timeout_ticks: 0 이면 무기한. 시간이 다 되면 Ntimeout (프로세스는 계속 실행 중).
 *   자기 자신을 기다리면 Ndeadlock. idle/부트 스레드가 부르면 잠들 수 없으므로 양보하며 폴링한다.
 */
Nstatus process_wait(process_t *proc, u64 timeout_ticks, i32 *out_code);

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
 * 평소에는 schedule()/프로세스 생성·종료 때 자동으로 수행되지만, 자원 회수를 정확히 확인해야 하는
 * 자체 점검이나 메모리 압박 시 명시적으로 부를 수 있다. 수거 대기 중인(zombie) 슬롯은 남는다.
 */
void process_reap_terminated(void);

/* asm 에서 호출: 스레드 함수가 반환했을 때의 종료 처리 */
void process_thread_exit(void) __attribute__((noreturn));

/* switch.s */
void cpu_switch_context(u64 *save_rsp, u64 new_rsp);

/* 부팅 시 한 번: CR0/CR4 에서 x87/SSE 를 사용자 모드가 쓸 수 있게 켠다 (OSFXSR, OSXMMEXCPT, NE, MP) */
void cpu_fpu_init(void);

/*
 * fork 의 자식 프로세스를 만든다 (READY). as 는 이미 복사된 사용자 주소 공간(소유권이 넘어간다),
 * frame 은 부모가 시스템 콜에 들어온 순간의 사용자 레지스터. 신원/umask/힙 경계/FPU 상태는 부모(current)에서.
 * 핸들 테이블은 호출자가 채운다 (nx_handles_inherit). 성공하면 *out_proc 에 참조 1 개.
 */
Nstatus process_create_forked(addr_space_t as, const struct trap_frame *frame, process_t **out_proc);

/* kernel/exec/fork_return.s: frame 의 레지스터로 사용자 모드에 돌아간다 */
void fork_return_to_user(struct trap_frame *frame) __attribute__((noreturn));
void process_entry_trampoline(void);

#endif /* KERNEL_PROCESS_H */
