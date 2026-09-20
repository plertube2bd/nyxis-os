/*
 * pit_base.h - 8254 PIT 타이머 드라이버
 */
#ifndef TIMER_BASE_H
#define TIMER_BASE_H

#include "nyxis.h"

/* PIT 기본 입력 클럭 (Hz) */
#define PIT_BASE_FREQ 1193182UL

/*
 * 타이머 초기화 (IRQ0 핸들러 등록 + PIC 마스크 해제)
 * frequency_hz : 인터럽트 주기 (Hz). 19 ~ 1193182 범위 (16비트 divisor 제약).
 *                예: 100 = 10ms 마다 tick
 * 반드시 interrupt_init() 와 pic_remap() 이후에 호출해야 한다.
 */
Nstatus timer_init(u32 frequency_hz);

/* 현재 tick 값 */
u64 timer_get_tick(void);

/*
 * 밀리초 단위 sleep (hlt 기반 대기).
 * 인터럽트가 켜져(sti) 있고 timer_init 이 끝난 상태에서만 동작한다.
 * 그렇지 않으면 영원히 대기하는 대신 즉시 반환한다.
 */
void sleep_ms(u64 ms);

/* 부팅 후 경과한 tick 수 (읽기 전용) */
extern volatile u64 timer_tick;

#endif /* TIMER_BASE_H */
