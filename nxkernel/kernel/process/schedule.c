/*
 * schedule.c - 스케줄러 (정책은 sched_rr.h 를 인라인으로 사용, 선점 규칙은 schedule.h 참고)
 *
 * 문맥 전환 도중 인터럽트로 상태가 바뀌지 않도록 인터럽트를 끈 채 선택한다.
 * 현재 프로세스가 TERMINATED 인 경우 READY 로 되돌리지 않는다. (process_switch 참고)
 */

#include "kernel/process/schedule.h"
#include "kernel/process/process.h"
#include "kernel/process/sched_rr.h"
#include "nyxis.h"

static volatile bool g_need_resched = false;

void schedule(void)
{
    process_t *next;
    u64 flags;

    if (!current_process || !process_list)
        return;

    flags = irq_save();

    g_need_resched = false;

    /* 이미 CPU 를 떠난 종료 프로세스의 자원(주소 공간)을 회수한다. 현재 프로세스(종료 중일 수 있음)는
     * 아직 그 스택/CR3 위에서 실행 중이므로 제외되고, 다음 schedule() 에서 회수된다. */
    process_reap_terminated();

    next = sched_pick_next(current_process, process_list);
    if (next)
        (void)process_switch(next);   /* 다시 이 스레드가 선택되면 여기로 복귀 (타임슬라이스는 그때 다시 채워진다) */

    irq_restore(flags);
}

void sched_request_resched(void)
{
    g_need_resched = true;
}

void sched_timer_tick(void)
{
    process_t *self = current_process;

    /* idle(pid 0)은 사용자 모드가 아니라서 선점 대상이 아니다. idle 루프는 스스로 schedule() 한다. */
    if (!self || self->pid == 0)
        return;

    if (self->slice_ticks > 0)
        self->slice_ticks--;
    if (self->slice_ticks == 0)
        g_need_resched = true;
}

void sched_preempt_point(void)
{
    if (!g_need_resched)
        return;

    /* 아무도 READY 가 아니면 schedule() 이 그냥 돌아온다 — 그때는 같은 프로세스가 새 타임슬라이스로 계속 */
    if (current_process)
        current_process->slice_ticks = SCHED_SLICE_TICKS;
    schedule();
}
