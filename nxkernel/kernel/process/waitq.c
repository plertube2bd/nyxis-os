/*
 * waitq.c - 대기 큐 구현 (설계와 사용 규칙은 waitq.h 참고)
 *
 * 타임아웃은 기존 sleep 과 같은 wake_tick 메커니즘을 쓴다: 스케줄러가 시간이 된 BLOCKED 프로세스를
 * READY 로 돌리면, 깨어난 쪽은 wait_woken 이 꺼져 있는 것을 보고 "누가 깨운 게 아니라 시간이 다 됐다"
 * 고 판단한다.
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

static void unlink_entry(waitq_entry_t *e)
{
    waitq_t *q = e->q;
    waitq_entry_t *cur;
    waitq_entry_t *prev = nNULL;

    for (cur = q->head; cur; prev = cur, cur = cur->next_in_q) {
        if (cur != e)
            continue;
        if (prev)
            prev->next_in_q = e->next_in_q;
        else
            q->head = e->next_in_q;
        if (q->tail == e)
            q->tail = prev;
        break;
    }
    e->next_in_q = nNULL;
}

void waitq_cancel_locked(process_t *p)
{
    waitq_entry_t *e = p->wait_entries;

    while (e) {
        waitq_entry_t *next = e->next_of_proc;

        unlink_entry(e);
        e->next_of_proc = nNULL;
        e = next;
    }
    p->wait_entries = nNULL;
}

Nstatus waitq_sleep_multi_locked(waitq_t **qs, u32 n, u64 timeout_ticks)
{
    waitq_entry_t entries[WAITQ_MAX_MULTI];
    process_t *self = current_process;
    u32 i;
    u32 used = 0;

    if (!qs || n == 0 || n > WAITQ_MAX_MULTI)
        return NinvalidArg;
    if (!self || self->pid == 0)
        return Npermission;

    for (i = 0; i < n; i++) {
        u32 j;
        bool dup = false;

        if (!qs[i])
            return NinvalidArg;
        for (j = 0; j < i; j++) {
            if (qs[j] == qs[i])
                dup = true;
        }
        if (dup)
            continue;

        entries[used].proc = self;
        entries[used].q = qs[i];
        entries[used].next_in_q = nNULL;
        entries[used].next_of_proc = self->wait_entries;
        self->wait_entries = &entries[used];
        if (qs[i]->tail)
            qs[i]->tail->next_in_q = &entries[used];
        else
            qs[i]->head = &entries[used];
        qs[i]->tail = &entries[used];
        used++;
    }

    self->wait_woken = false;
    self->state = PROCESS_BLOCKED;
    self->wake_tick = timeout_ticks ? timer_get_tick() + timeout_ticks : 0;

    schedule();      /* 깨워지거나 시간이 되면 스케줄러가 READY 로 바꾸고 다시 이 프로세스를 골라 여기로 온다 */

    if (self->state == PROCESS_BLOCKED) {
        /* 전환이 일어나지 않았다 (idle 이 항상 있으므로 정상이라면 도달하지 않음) */
        waitq_cancel_locked(self);
        self->state = PROCESS_RUNNING;
        self->wake_tick = 0;
        return Ninterrupted;
    }

    if (!self->wait_woken) {
        /* 아무도 깨우지 않았고 wake_tick 이 지나 스케줄러가 깨웠다 */
        waitq_cancel_locked(self);
        return Ntimeout;
    }

    return NSTATUS_OK;               /* 깨운 쪽이 이미 모든 항목을 떼어 냈다 */
}

Nstatus waitq_sleep_locked(waitq_t *q, u64 timeout_ticks)
{
    return waitq_sleep_multi_locked(&q, 1, timeout_ticks);
}

static void wake_locked(waitq_entry_t *e)
{
    process_t *p = e->proc;

    waitq_cancel_locked(p);         /* 이 프로세스의 모든 항목을 모든 큐에서 뗀다 (e 포함) */
    p->wait_woken = true;
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
