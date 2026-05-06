#include "pic.h"
#include "lowlevel.h"
#include "nyxis.h"

// ============================
// PIC DEFINES
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

static u8 g_pic_offset1 = 0x20;
static u8 g_pic_offset2 = 0x28;

// ============================
// IMPLEMENTATION
// ============================

void pic_remap(i32 offset1, i32 offset2) {
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

void pic_send_eoi(u8 irq) {
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

void pic_disable(void) {
    // 모든 IRQ 마스크
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

static u16 __pic_get_reg(u8 ocw3) {
    // master/slave 둘 다 같은 명령
    outb(PIC1_CMD, ocw3);
    outb(PIC2_CMD, ocw3);

    u8 master = inb(PIC1_CMD);
    u8 slave  = inb(PIC2_CMD);

    return (slave << 8) | master;
}

u16 pic_get_irr(void) {
    return __pic_get_reg(PIC_READ_IRR);
}

u16 pic_get_isr(void) {
    return __pic_get_reg(PIC_READ_ISR);
}

u16 pic_get_mask(void) {
    u8 master = inb(PIC1_DATA);
    u8 slave  = inb(PIC2_DATA);

    return (slave << 8) | master;
}

void pic_set_mask(u8 irq) {
    u16 port;
    u8 value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }

    value = inb(port) | (1 << irq);
    outb(port, value);
}

void pic_clear_mask(u8 irq) {
    u16 port;
    u8 value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }

    value = inb(port) & ~(1 << irq);
    outb(port, value);
}

u8 pic_irq_to_vector(u8 irq) {
    if (irq < 8) {
        return g_pic_offset1 + irq;
    } else {
        return g_pic_offset2 + (irq - 8);
    }
}
