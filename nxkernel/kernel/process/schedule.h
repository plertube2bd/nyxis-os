/*
 * schedule.h - 스케줄러 진입점
 *
 * 모든 스케줄링은 schedule() 하나로 감싼다. 정책은 sched_rr.h 처럼 인라인 함수로
 * 분리되어 schedule.c 에서 가져다 쓴다. (라운드 로빈)
 *
 * [선점] 사용자 모드만 선점한다 (커널 코드는 지금처럼 비선점).
 *  - 타이머 IRQ 가 현재 프로세스의 타임슬라이스(SCHED_SLICE_TICKS)를 줄이고, 다 쓰면 "재스케줄 필요" 를 표시한다.
 *    대기 큐에서 누군가 깨어날 때도 표시한다.
 *  - 실제 전환은 IRQ 가 ring3 로 돌아가기 직전(EOI 를 보낸 뒤)에만 sched_preempt_point() 에서 일어난다.
 *    커널 코드는 중간에 끊기지 않으므로 VFS/드라이버의 락 규칙은 그대로 유지된다.
 *  - 그래서 사용자 프로그램이 무한 루프를 돌아도 시스템 전체가 멈추지 않는다.
 */
#ifndef KERNEL_PROCESS_SCHEDULE_H
#define KERNEL_PROCESS_SCHEDULE_H

#include "nyxis.h"

#define SCHED_SLICE_TICKS 5U    /* 100Hz 기준 50ms */

/* 다음에 실행할 프로세스를 골라 문맥 전환한다. 실행할 다른 프로세스가 없으면 그냥 반환한다. */
void schedule(void);

/* 다음 선점 지점에서 전환하도록 표시한다 (IRQ 핸들러에서도 안전) */
void sched_request_resched(void);

/* 타이머 IRQ 에서 매 tick 호출: 현재 프로세스의 타임슬라이스를 소모한다 */
void sched_timer_tick(void);

/*
 * 선점 지점: 인터럽트가 사용자 모드로 돌아가기 직전, EOI 를 보낸 뒤에 호출한다.
 * 재스케줄이 표시되어 있으면 다른 프로세스로 전환한다 (이 프로세스는 나중에 여기서 이어서 복귀).
 */
void sched_preempt_point(void);

#endif /* KERNEL_PROCESS_SCHEDULE_H */
