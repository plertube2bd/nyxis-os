#ifndef PIC_H
#define PIC_H

#include "nyxis.h"

static u8 g_pic_offset1;
static u8 g_pic_offset2;

// PIC 리맵
void pic_remap(i32 offset1, i32 offset2);

// IRQ 종료 신호 (EOI)
void pic_send_eoi(u8 irq);

// pic 사용하지 않기
void pic_disable(void);

// irr 얻기
u16 pic_get_irr(void);

// isr 얻기
u16 pic_get_isr(void);

// pic 인터럽트(irq)를 인터럽트 벡터로 변환
u8 pic_irq_to_vector(u8 irq);

// 현재 irq 마스크 상태
u16 pic_get_mask(void);

// irq 마스킹
void pic_set_mask(u8 irq);

// irq 언마스크
void pic_clear_mask(u8 irq);

static u16 __pic_get_reg(u8 ocw3);

#endif
