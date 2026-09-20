/*
 * pic.h - 8259 PIC (레거시 인터럽트 컨트롤러) 드라이버
 *
 * [수정 이력 요약]
 *  - 헤더에 static 변수/static 함수 선언이 있던 것을 제거했다.
 *    (이 헤더를 포함하는 모든 파일에서 "정의되지 않은 static 함수" / "사용되지 않는
 *     변수" 경고가 발생했고, -Werror 에서는 빌드 실패)
 *  - '__' 로 시작하는 식별자(예약됨)를 사용하지 않도록 변경.
 */
#ifndef PIC_H
#define PIC_H

#include "nyxis.h"

/* PIC 리맵: IRQ 0~7 -> offset1.., IRQ 8~15 -> offset2.. (모든 IRQ 는 마스크 상태 유지) */
void pic_remap(i32 offset1, i32 offset2);

/* IRQ 종료 신호 (EOI) */
void pic_send_eoi(u8 irq);

/* PIC 를 사용하지 않기 (모든 IRQ 마스크) */
void pic_disable(void);

/* IRR / ISR 얻기 */
u16 pic_get_irr(void);
u16 pic_get_isr(void);

/* 스퓨리어스 IRQ(7/15) 인지 확인 */
bool pic_is_spurious(u8 irq);

/* IRQ 번호 -> 인터럽트 벡터 */
u8 pic_irq_to_vector(u8 irq);

/* 현재 IRQ 마스크 (비트가 1 이면 마스크됨) */
u16 pic_get_mask(void);

/* IRQ 마스킹 / 언마스크 */
void pic_set_mask(u8 irq);
void pic_clear_mask(u8 irq);

#endif /* PIC_H */
