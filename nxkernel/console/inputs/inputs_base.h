/*
 * inputs_base.h - PS/2 키보드 드라이버 (인터럽트 방식, IRQ1)
 *
 * [수정 이력 요약]
 *  - 이 헤더는 원래 컴파일이 불가능했다 (정의되지 않은 inline 선언, static 변수의 extern 선언 등).
 *  - 폴링 API(get_scancode/get_char, 호출하는 곳 없음)를 IRQ1 기반 드라이버로 바꿨다. 키 입력은
 *    tty.c 의 줄 편집기로 들어가고, 사용자 프로그램은 NxRead(stdin) 으로 읽는다.
 */
#ifndef INPUTS_BASE_H
#define INPUTS_BASE_H

#include "nyxis.h"

/*
 * i8042 컨트롤러가 있으면 키보드 인터럽트(IRQ1)를 켜고 핸들러를 등록한다. 컨트롤러가 없으면
 * (포트가 0xFF 를 돌려주면) 아무것도 하지 않는다. 스캔코드 세트 1(컨트롤러 변환 켜짐) 기준.
 */
void keyboard_init(void);

#endif /* INPUTS_BASE_H */
