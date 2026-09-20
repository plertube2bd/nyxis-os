/*
 * schedule.c - 스케줄러 (정책은 sched_rr.h 를 인라인으로 사용)
 *
 * 문맥 전환 도중 인터럽트로 상태가 바뀌지 않도록 인터럽트를 끈 채 선택한다.
 * 현재 프로세스가 TERMINATED 인 경우 READY 로 되돌리지 않는다. (process_switch 참고)
 */

#include "kernel/process/schedule.h"
#include "kernel/process/process.h"
#include "kernel/process/sched_rr.h"
#include "nyxis.h"

void schedule(void)
{
    process_t *next;
    u64 flags;

    if (!current_process || !process_list)
        return;

    flags = irq_save();

    next = sched_pick_next(current_process, process_list);
    if (next)
        (void)process_switch(next);   /* 다시 이 스레드가 선택되면 여기로 복귀 */

    irq_restore(flags);
}
