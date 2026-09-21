/*
 * pit_base.c - 8254 PIT 타이머 드라이버
 *
 * [기존 코드의 문제 (전면 수정)]
 *  - lowlevel.h 와 같은 이름/시그니처의 outb/inb 를 static inline 으로 다시 정의 -> 재정의 오류.
 *  - timer_tick 을 헤더에서는 extern, 소스에서는 static 으로 선언 -> 링크/컴파일 오류.
 *  - PIC 리맵 코드를 복붙해서 pic 드라이버와 중복 (그리고 리맵 후 마스크를 그대로 복원해
 *    부팅 시 열려 있던 IRQ 가 새 벡터로 들어옴).
 *  - timer_interrupt_handler 를 "일반 함수" 로 IDT 에 직접 연결하려 했다. 일반 함수는
 *    ret 로 끝나므로 인터럽트에서 iretq 가 실행되지 않아 즉시 스택 붕괴.
 *  - idt_set_gate 시그니처가 interrupt.h 와 달랐다.
 *  - sleep_ms 가 tick 주기를 10ms(100Hz)로 하드코딩했다.
 *  이제 PIC/IDT 는 공용 드라이버(pic.c/interrupt.c)를 사용하고, 핸들러는 trap_frame 을
 *  받는 C 함수이다. iretq 는 isr_stubs.s 가 처리한다.
 */

#include "nyxis.h"
#include "interrupt.h"
#include "drivers/pic/pic.h"
#include "kernel/timer/pit/pit_base.h"

#define PIT_CMD   0x43
#define PIT_DATA  0x40

#define PIT_IRQ   0

volatile u64 timer_tick = 0;
static u32 g_timer_hz = 0;

static void pit_set_frequency(u32 hz)
{
    u32 divisor = (u32)(PIT_BASE_FREQ / hz);

    outb(PIT_CMD, 0x36);   /* 채널 0, lobyte/hibyte, 모드 3(square wave) */
    outb(PIT_DATA, (u8)(divisor & 0xFFU));
    outb(PIT_DATA, (u8)((divisor >> 8) & 0xFFU));
}

/* IRQ0 핸들러: EOI 는 interrupt_dispatch 가 보낸다 */
static void timer_interrupt_handler(struct trap_frame *frame)
{
    (void)frame;
    timer_tick++;
}

Nstatus timer_init(u32 frequency_hz)
{
    Nstatus status;

    /* divisor 는 1~65535: 19Hz 미만은 표현 불가, PIT_BASE_FREQ 초과도 불가 */
    if (frequency_hz < 19U || frequency_hz > PIT_BASE_FREQ)
        return NinvalidArg;

    g_timer_hz = frequency_hz;
    timer_tick = 0;

    status = irq_register_handler(PIT_IRQ, timer_interrupt_handler);
    if (NSTATUS_IS_ERR(status))
        return status;

    pit_set_frequency(frequency_hz);
    pic_clear_mask(PIT_IRQ);

    return NSTATUS_OK;
}

u64 timer_get_tick(void)
{
    return timer_tick;
}

u32 timer_get_hz(void)
{
    return g_timer_hz;
}

u64 timer_get_ns(void)
{
    u64 ticks = timer_tick;
    u64 sec;
    u64 rem;

    if (g_timer_hz == 0)
        return 0;

    /* ticks * 1e9 를 바로 곱하면 약 5년 뒤 오버플로하므로 초/나머지로 나눠 계산 */
    sec = ticks / g_timer_hz;
    rem = ticks % g_timer_hz;
    return sec * 1000000000UL + (rem * 1000000000UL) / g_timer_hz;
}

void sleep_ms(u64 ms)
{
    u64 start;
    u64 wait_ticks;

    if (g_timer_hz == 0 || !(read_rflags() & RFLAGS_IF))
        return;     /* 타이머가 없거나 인터럽트가 꺼져 있으면 영원히 대기하게 되므로 반환 */

    /* 올림 계산. ms 가 너무 커서 오버플로하지 않도록 상한을 둔다 (약 1년) */
    if (ms > 31536000000UL)
        ms = 31536000000UL;
    wait_ticks = (ms * g_timer_hz + 999UL) / 1000UL;

    start = timer_tick;
    while ((timer_tick - start) < wait_ticks)
        hlt();
}
