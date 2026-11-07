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
#include "kernel/process/waitq.h"

/* Process list */
process_t *process_list = nNULL;

/* Current process */
process_t *current_process = nNULL;

/*
 * 커널 스택: 슬롯마다 [가드 4KiB (매핑 해제)][스택 PROCESS_KSTACK_SIZE]. 페이지 정렬이어야
 * 가드 페이지만 정확히 매핑 해제할 수 있다 (process_init 참고).
 */
NX_STATIC_ASSERT(kstack_guard_is_page, PROCESS_KSTACK_GUARD == PAGE_SIZE);
NX_STATIC_ASSERT(kstack_size_is_pages, (PROCESS_KSTACK_SIZE % PAGE_SIZE) == 0);

static process_t g_procs[PROCESS_MAX];
static u8 g_kstacks[PROCESS_MAX][PROCESS_KSTACK_GUARD + PROCESS_KSTACK_SIZE] __attribute__((aligned(4096)));
static u32 g_next_pid = 1;
static i32 g_last_exit_code = 0;

/* 슬롯의 커널 스택 최하위 주소 (가드 페이지 바로 위) */
static u8 *kstack_bottom(u32 slot)
{
    return &g_kstacks[slot][PROCESS_KSTACK_GUARD];
}

/* 리스트에서 p 를 떼어내고 슬롯을 비운다 (인터럽트가 꺼진 상태에서 호출) */
static void free_slot_locked(process_t *p, process_t *prev)
{
    if (prev)
        prev->next = p->next;
    else
        process_list = p->next;

    memset(p, 0, sizeof(*p));      /* in_use = false */
}

/*
 * 종료된 프로세스들을 정리한다 (인터럽트가 꺼진 상태에서 호출).
 *  - 자원(전용 주소 공간)은 항상 즉시 회수한다.
 *  - 슬롯은 아무도 참조하지 않을 때(refs == 0)만 회수한다. 프로세스 핸들 등이 참조하는 동안은
 *    종료 코드를 읽을 수 있도록 남긴다.
 * 현재 실행 중인 프로세스는 제외한다: 아직 그 커널 스택과 CR3 위에서 실행 중일 수 있다.
 */
static void process_reap(void)
{
    process_t *p = process_list;
    process_t *prev = nNULL;

    while (p) {
        process_t *next = p->next;

        if (p->state == PROCESS_TERMINATED && p != current_process && p->pid != 0) {
            /* TERMINATED 이고 current_process 도 아니므로 이미 다른 프로세스로 전환된 뒤이다 —
             * CR3 로 실려 있지 않으므로 주소 공간(ELF 세그먼트 + 힙 + 스택)을 안전하게 회수할 수 있다. */
            if (p->addrspace.pml4)
                paging_addrspace_destroy(&p->addrspace);

            if (p->refs == 0) {
                free_slot_locked(p, prev);
                p = next;
                continue;           /* prev 는 그대로 */
            }
        }

        prev = p;
        p = next;
    }
}

static bool pid_in_use_locked(u32 pid)
{
    process_t *p;

    for (p = process_list; p; p = p->next) {
        if (p->in_use && p->pid == pid)
            return true;
    }
    return false;
}

/* 새 pid. 32비트가 한 바퀴 돌아도 0(idle) 과 사용 중인 pid 는 주지 않는다 (슬롯은 최대 PROCESS_MAX 개) */
static u32 alloc_pid_locked(void)
{
    for (;;) {
        u32 pid = g_next_pid++;

        if (g_next_pid == 0)
            g_next_pid = 1;
        if (pid != 0 && !pid_in_use_locked(pid))
            return pid;
    }
}

Nstatus process_init(void)
{
    process_t *idle = &g_procs[0];
    u32 i;

    if (process_list)
        return NalreadyInitialized;

    memset(g_procs, 0, sizeof(g_procs));

    /* 모든 커널 스택 아래의 가드 페이지를 매핑 해제한다 (paging_enable() 이후여야 한다) */
    for (i = 0; i < PROCESS_MAX; i++) {
        Nstatus status = paging_unmap_kernel_page(g_kstacks[i]);

        if (NSTATUS_IS_ERR(status))
            return status;
    }

    /* 부트 스레드 = idle 프로세스. 스택은 start.s 의 부트 스택을 계속 사용한다. */
    idle->pid = 0;
    idle->uid = 0;
    idle->gid = 0;
    memcpy(idle->user_name, "root", 5);
    idle->state = PROCESS_RUNNING;
    idle->in_use = true;
    idle->kernel_stack = nNULL;
    idle->next = nNULL;
    nx_handles_init(&idle->handles);

    process_list = idle;
    current_process = idle;

    return NSTATUS_OK;
}

/*
 * 빈 슬롯을 하나 잡아 공통 필드와 초기 커널 스택을 구성하고 리스트 끝에 매단다
 * (인터럽트가 꺼진 상태에서 호출). 빈 슬롯이 없으면 NULL.
 *
 * 초기 커널 스택 구성 (switch.s 주석 참고). top 은 16바이트 정렬.
 *   top-8  : 반환 주소 = process_entry_trampoline
 *   top-16 : rbp, top-24 : rbx, top-32 : r12(진입 함수), top-40 : r13(인자),
 *   top-48 : r14, top-56 : r15   <- 저장된 rsp
 */
static process_t *alloc_process_locked(process_entry_t entry, void *arg)
{
    u32 i;
    u32 slot = PROCESS_MAX;
    process_t *proc;
    process_t *tail;
    u64 *sp;

    process_reap();

    for (i = 1; i < PROCESS_MAX; i++) {
        if (!g_procs[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot == PROCESS_MAX)
        return nNULL;

    proc = &g_procs[slot];
    memset(proc, 0, sizeof(*proc));
    memset(kstack_bottom(slot), 0, PROCESS_KSTACK_SIZE);   /* 가드 페이지는 건드리지 않는다 */

    proc->pid = alloc_pid_locked();
    proc->parent_pid = current_process ? current_process->pid : 0;

    /* 신원 상속 (토큰으로 바꾸는 것은 process_spawn 이 생성 직후, 실행 전에 한다) */
    if (current_process) {
        proc->uid = current_process->uid;
        proc->gid = current_process->gid;
        memcpy(proc->user_name, current_process->user_name, sizeof(proc->user_name));
    }
    proc->state = PROCESS_READY;
    proc->entry_point = entry;
    proc->kernel_stack = kstack_bottom(slot);
    proc->slice_ticks = SCHED_SLICE_TICKS;
    proc->in_use = true;
    waitq_init(&proc->exit_waiters);
    nx_handles_init(&proc->handles);

    sp = (u64 *)(usize)((u64)(usize)kstack_bottom(slot) + PROCESS_KSTACK_SIZE);
    *--sp = (u64)(usize)process_entry_trampoline;  /* 반환 주소 */
    *--sp = 0;                                     /* rbp */
    *--sp = 0;                                     /* rbx */
    *--sp = (u64)(usize)entry;                     /* r12 */
    *--sp = (u64)(usize)arg;                       /* r13 */
    *--sp = 0;                                     /* r14 */
    *--sp = 0;                                     /* r15 */
    proc->kernel_rsp = (u64)(usize)sp;

    tail = process_list;
    while (tail->next)
        tail = tail->next;
    tail->next = proc;

    return proc;
}

Nstatus process_create(process_entry_t entry_point, void *stack)
{
    process_t *proc;
    u64 flags;

    if (!entry_point)
        return NinvalidArg;
    if (!process_list)
        return NnotInitialized;

    flags = irq_save();

    proc = alloc_process_locked(entry_point, stack);
    if (!proc) {
        irq_restore(flags);
        return NprocessFailed;
    }
    proc->stack = stack;           /* refs = 0: 커널 스레드는 아무도 참조하지 않으므로 종료 즉시 회수 */

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
    process_t **out_proc
) {
    process_t *proc;
    u64 flags;

    if (!as.pml4 || !out_proc)
        return NinvalidArg;
    if (!process_list)
        return NnotInitialized;

    flags = irq_save();

    proc = alloc_process_locked(user_process_entry_thunk, nNULL);
    if (!proc) {
        irq_restore(flags);
        return NprocessFailed;
    }
    proc->addrspace = as;
    proc->user_entry = entry_vaddr;
    proc->user_stack_top = stack_top_vaddr;
    proc->heap_start = heap_start_vaddr;
    proc->heap_brk = heap_start_vaddr;
    proc->refs = 1;                /* 호출자의 참조. 호출자가 process_release() 할 때까지 슬롯 유지 */

    *out_proc = proc;

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

    /* 포트 권한은 프로세스마다 다르다 (상속하지 않는다: alloc 시 0) */
    gdt_set_io_permission(proc->io_base, proc->io_count);

    /* 이 프로세스에서 ring3 -> ring0 진입 시 사용할 커널 스택 */
    if (proc->kernel_stack)
        gdt_set_kernel_stack((u64)(usize)proc->kernel_stack + PROCESS_KSTACK_SIZE);

    current_process = proc;
    proc->state = PROCESS_RUNNING;
    proc->slice_ticks = SCHED_SLICE_TICKS;

    cpu_switch_context(&old->kernel_rsp, proc->kernel_rsp);

    /* 이 컨텍스트가 다시 선택되면 여기서 이어서 실행된다 */
    irq_restore(flags);
    return NSTATUS_OK;
}

/*
 * proc 를 종료 코드 code 로 종료한다 (인터럽트가 꺼진 상태에서 호출). 현재 프로세스면 반환하지 않는다.
 * 잠든 프로세스(대기 큐/수면)도 종료할 수 있다: 대기 큐에서 빼고 커널 스택은 버린다 (waitq.h 의 제약 참고).
 */
static Nstatus terminate_locked(process_t *proc, i32 code)
{
    if (proc->state == PROCESS_TERMINATED)
        return NSTATUS_OK;                     /* 이미 끝났다: 할 일 없음 (종료 코드는 처음 것을 유지) */

    proc->exit_code = code;
    g_last_exit_code = code;
    waitq_cancel_locked(proc);
    proc->wake_tick = 0;
    proc->state = PROCESS_TERMINATED;

    /* 프로세스가 가진 모든 핸들(열린 파일, 자식 프로세스 핸들 포함)을 닫는다. 자식 핸들이 닫히면
     * 아무도 참조하지 않게 된 종료된 자식은 바로 회수되고, 살아 있는 자식은 끝나는 즉시 회수된다. */
    nx_handles_close_all(&proc->handles);

    /* 이 프로세스를 기다리던 모두를 깨운다 (NxProcessWait) */
    (void)waitq_wake_all(&proc->exit_waiters);

    if (proc == current_process) {
        /* 자기 자신을 종료: 다른 프로세스로 전환하며 반환하지 않는다 */
        schedule();
        kernel_panic_simple("process_terminate: no runnable process", NprocessFailed);
    }

    process_reap();
    return NSTATUS_OK;
}

Nstatus process_terminate(u32 pid)
{
    process_t *proc;
    Nstatus status;
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

    status = terminate_locked(proc, (i32)NX_EXIT_KILLED);
    irq_restore(flags);
    return status;
}

Nstatus process_kill(process_t *proc, i32 code)
{
    Nstatus status;
    u64 flags;

    if (!proc || !proc->in_use)
        return NinvalidArg;
    if (proc->pid == 0)
        return Npermission;

    flags = irq_save();
    status = terminate_locked(proc, code);
    irq_restore(flags);
    return status;
}

void process_exit_with_code(i32 code)
{
    if (!current_process || current_process->pid == 0)
        kernel_panic_simple("process_exit called on idle/boot thread", NinvalidState);

    (void)irq_save();
    (void)terminate_locked(current_process, code);
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

void process_retain(process_t *proc)
{
    u64 flags = irq_save();

    proc->refs++;
    irq_restore(flags);
}

void process_release(process_t *proc)
{
    u64 flags = irq_save();

    if (proc->refs == 0)
        kernel_panic_simple("process_release: reference count underflow", NinvalidState);
    proc->refs--;
    if (proc->refs == 0)
        process_reap();            /* 이미 종료되었다면 슬롯까지 지금 회수 */

    irq_restore(flags);
}

Nstatus process_wait(process_t *proc, u64 timeout_ticks, i32 *out_code)
{
    Nstatus status = NSTATUS_OK;
    u64 deadline;
    u64 flags;

    if (!proc || !out_code || !proc->in_use)
        return NinvalidArg;
    if (proc == current_process)
        return Ndeadlock;          /* 자기 자신의 종료를 기다리면 영원히 끝나지 않는다 */

    flags = irq_save();
    deadline = timeout_ticks ? timer_get_tick() + timeout_ticks : 0;

    while (proc->state != PROCESS_TERMINATED) {
        u64 now = timer_get_tick();

        if (deadline && now >= deadline) {
            status = Ntimeout;
            break;
        }

        if (!current_process || current_process->pid == 0) {
            /* idle/부트 스레드는 잠들 수 없다: 양보하고, 그래도 아니면 다음 인터럽트까지 쉰다 */
            schedule();
            if (proc->state != PROCESS_TERMINATED)
                __asm__ volatile ("sti\n\thlt\n\tcli" : : : "memory");
            continue;
        }

        status = waitq_sleep_locked(&proc->exit_waiters, deadline ? deadline - now : 0);
        if (status == Ntimeout)
            continue;              /* 루프 위에서 다시 판정 (그 사이에 끝났을 수 있다) */
        if (NSTATUS_IS_ERR(status))
            break;
    }

    if (proc->state == PROCESS_TERMINATED) {
        *out_code = proc->exit_code;
        status = NSTATUS_OK;
    }

    irq_restore(flags);
    return status;
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
