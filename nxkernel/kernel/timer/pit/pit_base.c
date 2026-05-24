// ============================
// Simple PIT Timer Driver
// x86 bare-metal OS
// ============================

#include "nyxis.h"
#include "pit_base.h"

// ============================
// I/O PORT ACCESS
// ============================

static inline void outb(u16 port, u8 val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ============================
// PIC (Interrupt Controller)
// ============================

#define PIC1        0x20
#define PIC2        0xA0
#define PIC1_CMD    PIC1
#define PIC1_DATA   (PIC1+1)
#define PIC2_CMD    PIC2
#define PIC2_DATA   (PIC2+1)

#define ICW1_INIT   0x10
#define ICW1_ICW4   0x01
#define ICW4_8086   0x01

static void pic_remap(i32 offset1, i32 offset2) {
    u8 a1 = inb(PIC1_DATA);
    u8 a2 = inb(PIC2_DATA);

    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);

    outb(PIC1_DATA, (u8)offset1);
    outb(PIC2_DATA, (u8)offset2);

    outb(PIC1_DATA, 4);
    outb(PIC2_DATA, 2);

    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    outb(PIC1_DATA, a1);
    outb(PIC2_DATA, a2);
}

// ============================
// PIT (Programmable Interval Timer)
// ============================

#define PIT_CMD   0x43
#define PIT_DATA  0x40

static volatile u64 timer_tick = 0;

// 기본 1193182 Hz
#define PIT_BASE_FREQ 1193182

static void pit_set_frequency(u32 hz) {
    u32 divisor = PIT_BASE_FREQ / hz;

    outb(PIT_CMD, 0x36); // channel 0, square wave

    outb(PIT_DATA, (u8)(divisor & 0xFF));
    outb(PIT_DATA, (u8)((divisor >> 8) & 0xFF));
}

// ============================
// IDT HOOK (외부 연결 필요)
// ============================
// 실제 OS에서는 IDT에 IRQ0 = 32번 등록 필요

extern void idt_set_gate(i32 n, void (*handler)());

// ============================
// IRQ / INTERRUPT HANDLER
// ============================

static inline void pic_send_eoi(u8 irq) {
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

// IRQ0 = PIT
void timer_interrupt_handler() {
    timer_tick++;

    // EOI 보내기 (중요)
    pic_send_eoi(0);
}

// ============================
// PUBLIC API
// ============================

// 초기화 함수
void timer_init(u32 frequency_hz) {
    pic_remap(0x20, 0x28);      // IRQ 0~15 -> int 32~47
    pit_set_frequency(frequency_hz);

    // IDT에 IRQ0 등록 (vector 32)
    idt_set_gate(32, timer_interrupt_handler);
}

// ============================
// UTIL FUNCTIONS
// ============================

u64 timer_get_tick() {
    return timer_tick;
}

// ms 단위 sleep (busy wait)
// PIT frequency 기반 근사
void sleep_ms(u64 ms) {
    u64 start = timer_tick;
    u64 wait_ticks = ms / 10; // 예: 100Hz 기준 (10ms tick)

    while ((timer_tick - start) < wait_ticks) {
        __asm__ volatile ("hlt");
    }
}