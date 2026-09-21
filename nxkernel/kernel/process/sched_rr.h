/*
 * sched_rr.h - 라운드 로빈 스케줄링 정책 (schedule() 이 인라인으로 가져다 쓴다)
 *
 * 스케줄러 구조 (프로젝트 방침):
 *   - 외부에 공개되는 진입점은 schedule() 하나뿐이다.
 *   - 정책(누구를 다음에 실행할지)은 이 파일처럼 static __inline__ 함수로 분리하고
 *     schedule.c 가 #include 해서 사용한다. 나중에 정책을 바꿀 때는 이 헤더만
 *     다른 정책 헤더로 교체하면 되고, 호출자(schedule() 사용처)는 바뀌지 않는다.
 */
#ifndef KERNEL_PROCESS_SCHED_RR_H
#define KERNEL_PROCESS_SCHED_RR_H

#include "kernel/process/process.h"
#include "kernel/timer/pit/pit_base.h"

/*
 * current 다음부터 리스트를 정확히 한 바퀴 돌며 READY 인 프로세스를 찾는다.
 * 없으면 NULL.
 */
static __inline__ process_t *sched_pick_next(process_t *current, process_t *list_head)
{
    process_t *proc = current->next ? current->next : list_head;

    while (proc != current) {
        /* 잠든(sleep) 프로세스는 깨어날 시간이 되면 READY 로 되돌린다 */
        if (proc->in_use && proc->state == PROCESS_BLOCKED && proc->wake_tick != 0 &&
            timer_get_tick() >= proc->wake_tick) {
            proc->state = PROCESS_READY;
            proc->wake_tick = 0;
        }

        if (proc->in_use && proc->state == PROCESS_READY)
            return proc;
        proc = proc->next ? proc->next : list_head;
    }

    return (process_t *)0;
}

#endif /* KERNEL_PROCESS_SCHED_RR_H */
