// ============================
// Simple PIT Timer Driver
// Header File
// ============================

#ifndef TIMER_BASE_H
#define TIMER_BASE_H

#include "nyxis.h"

// ============================
// CORE API
// ============================

/**
 * 타이머 초기화
 * @param frequency_hz : 인터럽트 주기 (Hz)
 * 예: 100Hz = 10ms tick
 */
void timer_init(u32 frequency_hz);

/**
 * 현재 tick 값 반환
 */
u64 timer_get_tick(void);

/**
 * 밀리초 단위 sleep (busy wait 기반)
 * @param ms : 대기 시간
 */
void sleep_ms(u64 ms);

// ============================
// INTERNAL (커널 전용)
// ============================

/**
 * IRQ0 (PIT) 인터럽트 핸들러
 * IDT에서 직접 연결됨
 */
void timer_interrupt_handler(void);

// ============================
// GLOBAL TICK (외부 읽기 전용)
// ============================

extern volatile u64 timer_tick;

#endif // TIMER_H