/*
 * schedule.h - 스케줄러 (협력형 라운드 로빈)
 */
#ifndef KERNEL_PROCESS_SCHEDULE_H
#define KERNEL_PROCESS_SCHEDULE_H

void round_robin_schedule(void);
void schedule(void);

#endif /* KERNEL_PROCESS_SCHEDULE_H */
