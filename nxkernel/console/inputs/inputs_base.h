/*
 * inputs_base.h - PS/2 키보드 드라이버 (인터럽트 방식, IRQ1)
 *
 * [수정 이력 요약]
 *  - 이 헤더는 원래 컴파일이 불가능했다 (정의되지 않은 inline 선언, static 변수의 extern 선언 등).
 *  - 폴링 API(get_scancode/get_char, 호출하는 곳 없음)를 IRQ1 기반 드라이버로 바꿨다.
 *  - 키 이벤트: 모든 키의 누름/뗌을 수정자 상태(Shift/Ctrl/Alt/Super/AltGr/Caps)와 함께 struct nx_keyevent 로
 *    만든다. 평소에는 누름만 콘솔 터미널(tty.c)의 줄 편집기로 간다 (사용자 프로그램은 NxRead(stdin) 으로 읽는다).
 *    dev:/console 이 열려 있으면(keyboard_claim) 이벤트가 그 프로세스에게만 간다.
 */
#ifndef INPUTS_BASE_H
#define INPUTS_BASE_H

#include "nyxis.h"
#include "nyx_abi.h"
#include "kernel/process/waitq.h"

/*
 * i8042 컨트롤러가 있으면 키보드 인터럽트(IRQ1)를 켜고 핸들러를 등록한다. 컨트롤러가 없으면
 * (포트가 0xFF 를 돌려주면) 아무것도 하지 않는다. 스캔코드 세트 1(컨트롤러 변환 켜짐) 기준.
 */
void keyboard_init(void);

/* 키 이벤트를 콘솔 터미널 대신 이벤트 큐로 보낸다 (on) / 되돌린다. 바꿀 때 큐는 비운다 */
void keyboard_claim(bool on);

/* 큐에서 최대 max 개를 꺼낸다 (잠들지 않는다). 꺼낸 개수 */
u32 keyboard_take_events(struct nx_keyevent *out, u32 max);

/* NxPoll / 블로킹 읽기 */
bool keyboard_events_ready(void);
waitq_t *keyboard_waitq(void);

#endif /* INPUTS_BASE_H */
