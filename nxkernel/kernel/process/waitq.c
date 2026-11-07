/*
 * waitq.c - 대기 큐 구현 (설계와 사용 규칙은 waitq.h 참고)
 *
 * 프로세스는 동시에 최대 하나의 큐에서만 기다린다 (process_t.wq / wq_next 로 큐에 직접 매달린다 —
 * 메모리 할당이 필요 없다). 타임아웃은 기존 sleep 과 같은 wake_tick 메커니즘을 쓴다: 스케줄러가
 * 시간이 된 BLOCKED 프로세스를 READY 로 돌리면, 깨어난 쪽은 자신이 아직 큐에 남아 있는 것을 보고
 * "누가 깨운 게 아니라 시간이 다 됐다" 고 판단한다.
 */

#include "kernel/process/waitq.h"
#include "kernel/process/process.h"
#include "kernel/timer/pit/pit_base.h"
#include "lowlevel.h"

void waitq_init(waitq_t *q)
{
    q->head = nNULL;
    q->tail = nNULL;
}

static void enqueue(waitq_t *q, process_t *p)
{
    p->wq = q;
    p->wq_next = nNULL;
    if (q->tail)
        q->tail->wq_next = p;
    else
        q->head = p;
    q->tail = p;
}

void waitq_remove_locked(process_t *p)
{
    waitq_t *q = p->wq;
    process_t *cur;
    process_t *prev = nNULL;

    if (!q)
        return;

    for (cur = q->head; cur; prev = cur, cur = cur->wq_next) {
        if (cur != p)
            continue;
        if (prev)
            prev->wq_next = p->wq_next;
        else
            q->head = p->wq_next;
        if (q->tail == p)
            q->tail = prev;
        break;
    }

    p->wq = nNULL;
    p->wq_next = nNULL;
}

Nstatus waitq_sleep_locked(waitq_t *q, u64 timeout_ticks)
{
    process_t *self = current_process;

    if (!q)
        return NinvalidArg;
    if (!self || self->pid == 0)
        return Npermission;

    enqueue(q, self);
    self->state = PROCESS_BLOCKED;
    self->wake_tick = timeout_ticks ? timer_get_tick() + timeout_ticks : 0;

    schedule();      /* 깨워지거나 시간이 되면 스케줄러가 READY 로 바꾸고 다시 이 프로세스를 골라 여기로 온다 */

    if (self->state == PROCESS_BLOCKED) {
        /* 전환이 일어나지 않았다 (idle 이 항상 있으므로 정상이라면 도달하지 않음) */
        waitq_remove_locked(self);
        self->state = PROCESS_RUNNING;
        self->wake_tick = 0;
        return Ninterrupted;
    }

    if (self->wq) {
        /* 아직 큐에 있다 = 아무도 깨우지 않았고 wake_tick 이 지나 스케줄러가 깨웠다 */
        waitq_remove_locked(self);
        return Ntimeout;
    }

    return NSTATUS_OK;
}

static void wake_locked(process_t *p)
{
    waitq_remove_locked(p);
    p->wake_tick = 0;
    if (p->state == PROCESS_BLOCKED)
        p->state = PROCESS_READY;
    sched_request_resched();        /* 깨어난 프로세스가 빨리 실행되도록 (사용자 모드 복귀 시점에 전환) */
}

u32 waitq_wake_all(waitq_t *q)
{
    u64 flags = irq_save();
    u32 n = 0;

    while (q->head) {
        wake_locked(q->head);
        n++;
    }

    irq_restore(flags);
    return n;
}

bool waitq_wake_one(waitq_t *q)
{
    u64 flags = irq_save();
    bool woke = false;

    if (q->head) {
        wake_locked(q->head);
        woke = true;
    }

    irq_restore(flags);
    return woke;
}
