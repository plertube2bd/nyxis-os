#ifndef PIC_H
#define PIC_H

#include "nyxis.h"

// PIC 리맵
void pic_remap(i32 offset1, i32 offset2);

// IRQ 종료 신호 (EOI)
void pic_send_eoi(u8 irq);

#endif