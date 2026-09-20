/*
 * graphics_base.h - 프레임버퍼 기본 그리기 API
 *
 * [수정 이력 요약]
 *  - 헤더에 static inline 구현이 있었고 graphics_base.c 에도 똑같은 코드가 복사되어 있었다.
 *    (게다가 .c 는 정의되지 않은 memcopy 를 호출해서 컴파일 불가) -> 헤더는 선언만, 구현은 .c 로.
 *  - 좌표/길이 검사 없음으로 프레임버퍼 밖을 쓰던 문제와, draw_hline_fast 의
 *    u32 buffer[1024] 스택 버퍼 오버플로(length > 1024)를 수정.
 */
#ifndef GRAPHICS_BASE_H
#define GRAPHICS_BASE_H

#include "nyxis.h"
#include "memory.h"

/* 화면 안이면 (x, y) 픽셀을 color 로 칠한다. 밖이면 조용히 무시한다. */
void draw_pixel(NTBLI *info, i32 x, i32 y, u32 color);

/* 브레젠험 직선. 화면 밖 픽셀은 무시한다. */
void draw_line_plus(NTBLI *info, i32 x0, i32 y0, i32 x1, i32 y1, u32 color);

/* 수평선. 화면 밖 부분은 잘라낸다. */
void draw_hline_fast(NTBLI *info, i32 x, i32 y, i32 length, u32 color);

#endif /* GRAPHICS_BASE_H */
