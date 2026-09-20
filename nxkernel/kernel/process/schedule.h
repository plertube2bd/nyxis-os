/*
 * schedule.h - 스케줄러 진입점
 *
 * 모든 스케줄링은 schedule() 하나로 감싼다. 정책은 sched_rr.h 처럼 인라인 함수로
 * 분리되어 schedule.c 에서 가져다 쓴다. (초기에는 협력형 라운드 로빈)
 */
#ifndef KERNEL_PROCESS_SCHEDULE_H
#define KERNEL_PROCESS_SCHEDULE_H

/* 다음에 실행할 프로세스를 골라 문맥 전환한다. 실행할 다른 프로세스가 없으면 그냥 반환한다. */
void schedule(void);

#endif /* KERNEL_PROCESS_SCHEDULE_H */
