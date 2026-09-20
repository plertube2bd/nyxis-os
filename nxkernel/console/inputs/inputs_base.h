/*
 * inputs_base.h - PS/2 키보드 입력 (폴링 방식)
 *
 * [수정 이력 요약]
 *  - 이 헤더는 원래 컴파일이 불가능했다:
 *      * 정의되지 않은 static inline inb/io_wait 를 선언 (lowlevel.h 와 충돌)
 *      * 소스에서 static 으로 정의한 변수를 extern 으로 선언
 *      * translate() 의 선언/정의 타입 불일치 (char/uint8_t vs utf8/u8)
 *    내부 상태는 소스 파일 안에 숨기고 공개 API 3개만 남겼다.
 */
#ifndef INPUTS_BASE_H
#define INPUTS_BASE_H

#include "nyxis.h"

/* 키보드 컨트롤러에서 스캔코드 하나를 읽는다 (없으면 hlt 로 대기). 인터럽트가 켜져 있어야 함. */
u8 get_scancode(void);

/* 스캔코드(세트 1)를 ASCII 로 변환. 변환 불가하면 0. */
utf8 translate(u8 sc);

/* 인쇄 가능한 문자 하나를 얻을 때까지 대기 */
utf8 get_char(void);

#endif /* INPUTS_BASE_H */
