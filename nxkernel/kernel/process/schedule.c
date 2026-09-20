/*
 * schedule.c - 협력형 라운드 로빈 스케줄러
 *
 * [수정 이력 요약]
 *  - 후보 탐색을 "한 바퀴" 로 정확히 제한. (기존: 리스트가 원형이 아니면 무한 루프 가능)
 *  - 현재 프로세스가 TERMINATED 이면 READY 로 되돌리지 않는다.
 *  - 문맥 전환 도중 인터럽트로 상태가 바뀌지 않도록 인터럽트를 끈 채 선택한다.
 */

#include "kernel/process/schedule.h"
#include "kernel/process/process.h"
#include "nyxis.h"

void round_robin_schedule(void)
{
    process_t *start;
    process_t *proc;
    u64 flags;

    if (!current_process || !process_list)
        return;

    flags = irq_save();

    start = current_process;
    proc = start->next ? start->next : process_list;

    /* 리스트를 정확히 한 바퀴만 돈다 */
    while (proc != start) {
        if (proc->in_use && proc->state == PROCESS_READY) {
            (void)process_switch(proc);   /* 다시 이 스레드가 선택되면 여기로 복귀 */
            irq_restore(flags);
            return;
        }
        proc = proc->next ? proc->next : process_list;
    }

    /* 현재 프로세스가 종료 상태인데 갈 곳이 없다면 호출자가 처리한다 */
    irq_restore(flags);
}

/* Simple round-robin scheduler */
void schedule(void)
{
    round_robin_schedule();
}
