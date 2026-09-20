/*
 * pic.c - 8259 PIC 드라이버
 *
 * [왜 리맵이 필수인가]
 *  PIC 의 기본 벡터(IRQ0~7 -> 0x08~0x0F)는 CPU 예외 벡터(더블폴트=8 등)와 겹친다.
 *  리맵하지 않고 인터럽트를 켜면 타이머 IRQ 가 "더블폴트" 로 처리된다.
 *
 * [수정 이력 요약]
 *  - pic_remap 이 pic_irq_to_vector 용 오프셋을 갱신하지 않던 문제 수정.
 *  - 초기화 명령 사이에 io_wait() 추가 (구형 하드웨어 요구사항).
 *  - pic_remap 후 기존 마스크를 복원하지 않고 "전부 마스크" 로 시작하도록 변경.
 *    (부팅 직후 어떤 IRQ 도 원치 않게 들어오지 않게 하고, 필요한 드라이버가
 *     pic_clear_mask 로 명시적으로 켜도록 함)
 *  - 스퓨리어스 IRQ 판별 함수 추가.
 *  - C89 호환 / 정수 승격 캐스트 명시.
 */

#include "drivers/pic/pic.h"
#include "lowlevel.h"

#define PIC1         0x20
#define PIC2         0xA0
#define PIC1_CMD     PIC1
#define PIC1_DATA    (PIC1 + 1)
#define PIC2_CMD     PIC2
#define PIC2_DATA    (PIC2 + 1)

#define ICW1_INIT    0x10
#define ICW1_ICW4    0x01
#define ICW4_8086    0x01
#define PIC_EOI      0x20
#define PIC_READ_IRR 0x0A
#define PIC_READ_ISR 0x0B

static u8 g_pic_offset1 = 0x20;
static u8 g_pic_offset2 = 0x28;

void pic_remap(i32 offset1, i32 offset2)
{
    g_pic_offset1 = (u8)offset1;
    g_pic_offset2 = (u8)offset2;

    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();

    outb(PIC1_DATA, g_pic_offset1);   /* ICW2: 마스터 벡터 오프셋 */
    io_wait();
    outb(PIC2_DATA, g_pic_offset2);   /* ICW2: 슬레이브 벡터 오프셋 */
    io_wait();

    outb(PIC1_DATA, 4);               /* ICW3: 마스터의 IRQ2 에 슬레이브 연결 */
    io_wait();
    outb(PIC2_DATA, 2);               /* ICW3: 슬레이브 캐스케이드 ID */
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* 모든 IRQ 마스크 상태로 시작 */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_send_eoi(u8 irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_disable(void)
{
    /* 모든 IRQ 마스크 */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

static u16 pic_get_reg(u8 ocw3)
{
    u8 master;
    u8 slave;

    /* 마스터/슬레이브에 같은 명령을 보내고 각각 읽는다 */
    outb(PIC1_CMD, ocw3);
    outb(PIC2_CMD, ocw3);

    master = inb(PIC1_CMD);
    slave  = inb(PIC2_CMD);

    return (u16)(((u16)slave << 8) | master);
}

u16 pic_get_irr(void)
{
    return pic_get_reg(PIC_READ_IRR);
}

u16 pic_get_isr(void)
{
    return pic_get_reg(PIC_READ_ISR);
}

bool pic_is_spurious(u8 irq)
{
    u16 isr;

    /* 스퓨리어스 IRQ 는 각 PIC 의 마지막 입력(IRQ7 / IRQ15)으로만 발생한다.
     * 이때 ISR 의 해당 비트가 꺼져 있으면 가짜 인터럽트이다. */
    if (irq != 7 && irq != 15)
        return false;

    isr = pic_get_isr();
    return (isr & (u16)(1U << irq)) ? false : true;
}

u16 pic_get_mask(void)
{
    u8 master = inb(PIC1_DATA);
    u8 slave  = inb(PIC2_DATA);

    return (u16)(((u16)slave << 8) | master);
}

void pic_set_mask(u8 irq)
{
    u16 port;
    u8 value;

    if (irq >= 16)
        return;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq = (u8)(irq - 8);
    }

    value = (u8)(inb(port) | (1U << irq));
    outb(port, value);
}

void pic_clear_mask(u8 irq)
{
    u16 port;
    u8 value;

    if (irq >= 16)
        return;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq = (u8)(irq - 8);
        /* 슬레이브 IRQ 를 쓰려면 마스터의 캐스케이드(IRQ2)도 열려 있어야 한다 */
        outb(PIC1_DATA, (u8)(inb(PIC1_DATA) & ~(1U << 2)));
    }

    value = (u8)(inb(port) & ~(1U << irq));
    outb(port, value);
}

u8 pic_irq_to_vector(u8 irq)
{
    if (irq < 8)
        return (u8)(g_pic_offset1 + irq);

    return (u8)(g_pic_offset2 + (irq - 8));
}
