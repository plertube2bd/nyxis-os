/*
 * process.c - 프로세스/커널 스레드 테이블과 컨텍스트 전환 (설계는 process.h 참고)
 */

#include "kernel/process/process.h"
#include "kernel/paging/paging.h"
#include "kernel/error_handling/panic.h"
#include "interrupt.h"
#include "memory.h"
#include "kernel/timer/pit/pit_base.h"

/* Process list */
process_t *process_list = nNULL;

/* Current process */
process_t *current_process = nNULL;

static process_t g_procs[PROCESS_MAX];
static u8 g_kstacks[PROCESS_MAX][PROCESS_KSTACK_SIZE] __attribute__((aligned(16)));
static u32 g_next_pid = 1;
static i32 g_last_exit_code = 0;

/* 종료된 프로세스의 슬롯을 회수한다 (현재 실행 중인 것은 제외) */
static void process_reap(void)
{
    process_t *p = process_list;
    process_t *prev = nNULL;

    while (p) {
        process_t *next = p->next;

        if (p->state == PROCESS_TERMINATED && p != current_process && p->pid != 0) {
            if (prev)
                prev->next = next;
            else
                process_list = next;

            memset(p, 0, sizeof(*p));      /* in_use = false */
        } else {
            prev = p;
        }

        p = next;
    }
}

Nstatus process_init(void)
{
    process_t *idle = &g_procs[0];

    if (process_list)
        return NalreadyInitialized;

    memset(g_procs, 0, sizeof(g_procs));

    /* 부트 스레드 = idle 프로세스. 스택은 start.s 의 부트 스택을 계속 사용한다. */
    idle->pid = 0;
    idle->state = PROCESS_RUNNING;
    idle->in_use = true;
    idle->kernel_stack = nNULL;
    idle->next = nNULL;
    nx_handles_init(&idle->handles);

    process_list = idle;
    current_process = idle;

    return NSTATUS_OK;
}

Nstatus process_create(process_entry_t entry_point, void *stack)
{
    u32 i;
    u32 slot = PROCESS_MAX;
    process_t *proc;
    process_t *tail;
    u64 *sp;
    u64 top;
    u64 flags;

    if (!entry_point)
        return NinvalidArg;
    if (!process_list)
        return NnotInitialized;

    flags = irq_save();

    process_reap();

    for (i = 1; i < PROCESS_MAX; i++) {
        if (!g_procs[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot == PROCESS_MAX) {
        irq_restore(flags);
        return NprocessFailed;
    }

    proc = &g_procs[slot];
    memset(proc, 0, sizeof(*proc));
    memset(g_kstacks[slot], 0, PROCESS_KSTACK_SIZE);

    proc->pid = g_next_pid++;
    proc->state = PROCESS_READY;
    proc->stack = stack;
    proc->entry_point = entry_point;
    proc->cr3 = nNULL;
    proc->kernel_stack = g_kstacks[slot];
    proc->in_use = true;
    nx_handles_init(&proc->handles);

    /*
     * 초기 커널 스택 구성 (switch.s 주석 참고). top 은 16바이트 정렬.
     *   top-8  : 반환 주소 = process_entry_trampoline
     *   top-16 : rbp, top-24 : rbx, top-32 : r12(진입 함수), top-40 : r13(인자),
     *   top-48 : r14, top-56 : r15   <- 저장된 rsp
     */
    top = (u64)(usize)g_kstacks[slot] + PROCESS_KSTACK_SIZE;
    sp = (u64 *)(usize)top;
    *--sp = (u64)(usize)process_entry_trampoline;  /* 반환 주소 */
    *--sp = 0;                                     /* rbp */
    *--sp = 0;                                     /* rbx */
    *--sp = (u64)(usize)entry_point;               /* r12 */
    *--sp = (u64)(usize)stack;                     /* r13 */
    *--sp = 0;                                     /* r14 */
    *--sp = 0;                                     /* r15 */
    proc->kernel_rsp = (u64)(usize)sp;

    /* 리스트 끝에 추가 */
    tail = process_list;
    while (tail->next)
        tail = tail->next;
    tail->next = proc;

    irq_restore(flags);
    return NSTATUS_OK;
}

Nstatus process_switch(process_t *proc)
{
    process_t *old;
    u64 flags;

    if (!proc || !proc->in_use || proc->state != PROCESS_READY)
        return NinvalidArg;

    if (proc == current_process)
        return NSTATUS_OK;

    flags = irq_save();

    old = current_process;
    if (old && old->state == PROCESS_RUNNING)
        old->state = PROCESS_READY;

    /* 주소 공간 전환 (프로세스가 자기 CR3 를 가진 경우만) */
    if (proc->cr3)
        write_cr3((u64)(usize)proc->cr3);

    /* 이 프로세스에서 ring3 -> ring0 진입 시 사용할 커널 스택 */
    if (proc->kernel_stack)
        gdt_set_kernel_stack((u64)(usize)proc->kernel_stack + PROCESS_KSTACK_SIZE);

    current_process = proc;
    proc->state = PROCESS_RUNNING;

    cpu_switch_context(&old->kernel_rsp, proc->kernel_rsp);

    /* 이 컨텍스트가 다시 선택되면 여기서 이어서 실행된다 */
    irq_restore(flags);
    return NSTATUS_OK;
}

Nstatus process_terminate(u32 pid)
{
    process_t *proc;
    u64 flags;

    if (pid == 0)
        return Npermission;        /* idle(부트) 스레드는 종료 불가 */

    flags = irq_save();

    for (proc = process_list; proc; proc = proc->next) {
        if (proc->in_use && proc->pid == pid)
            break;
    }

    if (!proc || proc->state == PROCESS_TERMINATED) {
        irq_restore(flags);
        return NnotFound;
    }

    /* 프로세스가 가진 모든 핸들(열린 파일 포함)을 닫는다. 그러지 않으면 드라이버 자원이 새어 나간다 */
    nx_handles_close_all(&proc->handles);
    proc->state = PROCESS_TERMINATED;

    if (proc == current_process) {
        /* 자기 자신을 종료: 다른 프로세스로 전환하며 이 함수는 반환하지 않는다 */
        schedule();
        kernel_panic_simple("process_terminate: no runnable process", NprocessFailed);
    }

    process_reap();
    irq_restore(flags);
    return NSTATUS_OK;
}

void process_exit_with_code(i32 code)
{
    if (!current_process || current_process->pid == 0)
        kernel_panic_simple("process_exit called on idle/boot thread", NinvalidState);

    current_process->exit_code = code;
    g_last_exit_code = code;
    (void)process_terminate(current_process->pid);
    kernel_panic_simple("process_exit: unreachable", NkernelFault);
}

void process_exit(void)
{
    process_exit_with_code(0);
}

i32 process_last_exit_code(void)
{
    return g_last_exit_code;
}

Nstatus process_sleep_ticks(u64 ticks)
{
    process_t *self = current_process;
    u64 flags;

    if (!self || self->pid == 0)
        return Npermission;

    flags = irq_save();

    /* 최소 1 tick 은 잔다 (tick 경계 때문에 0 tick 이면 즉시 깨어나는 것과 같아진다) */
    self->wake_tick = timer_get_tick() + (ticks ? ticks : 1UL);
    self->state = PROCESS_BLOCKED;

    schedule();      /* 시간이 되면 스케줄러가 READY 로 바꾸고 다시 이 프로세스를 골라 여기로 돌아온다 */

    if (self->state == PROCESS_BLOCKED) {
        /* 아무도 실행할 수 없어서 전환이 일어나지 않았다 (idle 이 항상 있으므로 정상이라면 도달하지 않음) */
        self->state = PROCESS_RUNNING;
        self->wake_tick = 0;
        irq_restore(flags);
        return Ninterrupted;
    }

    irq_restore(flags);
    return NSTATUS_OK;
}

u32 process_count(void)
{
    process_t *p;
    u32 n = 0;

    for (p = process_list; p; p = p->next) {
        if (p->in_use && p->state != PROCESS_TERMINATED)
            n++;
    }
    return n;
}

void process_thread_exit(void)
{
    process_exit();
}
