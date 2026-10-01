/*
 * process.c - 프로세스/커널 스레드 테이블과 컨텍스트 전환 (설계는 process.h 참고)
 */

#include "kernel/process/process.h"
#include "kernel/paging/paging.h"
#include "kernel/mm/pfa.h"
#include "kernel/error_handling/panic.h"
#include "kernel/kernel.h"
#include "interrupt.h"
#include "memory.h"
#include "phys.h"
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

            /* 이 프로세스는 더 이상 CR3 로 실려 있지 않다고 보장된다 (TERMINATED 이고
             * current_process 도 아니므로 이미 다른 프로세스로 전환된 뒤이다). 그래야만
             * paging_addrspace_destroy() 로 전용 주소 공간(ELF 세그먼트 + 힙 + 스택)의
             * 모든 페이지를 안전하게 회수할 수 있다. */
            if (p->addrspace.pml4)
                paging_addrspace_destroy(&p->addrspace);

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

/*
 * process_entry_trampoline 이 이 함수를 커널 스레드 진입점처럼 호출한다(arg 는 쓰지 않는다).
 * current_process 는 이 시점에 이미 이 프로세스 자신으로 설정되어 있고, CR3 도
 * process_switch() 가 이 프로세스의 전용 주소 공간으로 이미 바꿔 둔 뒤이므로, ring3 진입에
 * 필요한 모든 정보를 process_t 자신에서 안전하게 읽을 수 있다 (부모의 호출 스택에 남은
 * 포인터를 넘겨받는 방식은 그 스택이 스케줄링 전에 이미 재사용될 수 있어 위험하다).
 */
static void user_process_entry_thunk(void *arg)
{
    process_t *self = current_process;

    (void)arg;
    enter_ring3((void *)(usize)self->user_entry, (void *)(usize)self->user_stack_top);
}

Nstatus process_create_user(
    addr_space_t as,
    u64 entry_vaddr,
    u64 stack_top_vaddr,
    u64 heap_start_vaddr,
    u32 *out_pid
) {
    u32 i;
    u32 slot = PROCESS_MAX;
    process_t *proc;
    process_t *tail;
    u64 *sp;
    u64 top;
    u64 flags;

    if (!as.pml4 || !out_pid)
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
    proc->stack = nNULL;
    proc->entry_point = user_process_entry_thunk;
    proc->addrspace = as;
    proc->user_entry = entry_vaddr;
    proc->user_stack_top = stack_top_vaddr;
    proc->heap_start = heap_start_vaddr;
    proc->heap_brk = heap_start_vaddr;
    proc->kernel_stack = g_kstacks[slot];
    proc->in_use = true;
    nx_handles_init(&proc->handles);

    /* 초기 커널 스택 구성은 process_create() 와 동일하다 (switch.s 주석 참고). */
    top = (u64)(usize)g_kstacks[slot] + PROCESS_KSTACK_SIZE;
    sp = (u64 *)(usize)top;
    *--sp = (u64)(usize)process_entry_trampoline;  /* 반환 주소 */
    *--sp = 0;                                     /* rbp */
    *--sp = 0;                                     /* rbx */
    *--sp = (u64)(usize)user_process_entry_thunk;  /* r12 */
    *--sp = 0;                                     /* r13 (thunk 은 arg 를 쓰지 않음) */
    *--sp = 0;                                     /* r14 */
    *--sp = 0;                                     /* r15 */
    proc->kernel_rsp = (u64)(usize)sp;

    tail = process_list;
    while (tail->next)
        tail = tail->next;
    tail->next = proc;

    *out_pid = proc->pid;

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

    /* 주소 공간 전환. 전용 주소 공간이 없는 커널 스레드는 "커널 전용"(사용자 영역이 없는)
     * 공유 주소 공간으로 전환한다 — CR3 를 절대 이전 프로세스 것으로 남겨두지 않는다.
     * (uaccess/시스템 콜은 항상 current_process->addrspace 를 검사하므로, 실제 CR3 가 그와
     * 다르면 소프트웨어 검사와 하드웨어 접근이 서로 다른 주소 공간을 보는 사고가 난다) */
    write_cr3(proc->addrspace.pml4 ? virt_to_phys(proc->addrspace.pml4) : paging_kernel_cr3());

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

void process_reap_terminated(void)
{
    u64 flags = irq_save();

    process_reap();
    irq_restore(flags);
}

Nstatus process_get_exit_code(u32 pid, i32 *out_code)
{
    process_t *p;
    u64 flags;

    if (!out_code)
        return NinvalidArg;

    flags = irq_save();

    for (p = process_list; p; p = p->next) {
        if (!p->in_use || p->pid != pid)
            continue;

        if (p->state != PROCESS_TERMINATED) {
            irq_restore(flags);
            return Nbusy;
        }

        *out_code = p->exit_code;
        irq_restore(flags);
        return NSTATUS_OK;
    }

    irq_restore(flags);
    return NnotFound;
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
