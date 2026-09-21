/*
 * printk.h - 커널 로그 출력 (프레임버퍼 + 시리얼)
 *
 * 지원 서식: %% %c %s %d %i %u %x %X %p %r  (길이 수정자: l, ll, z)
 *            폭/0 채움 (예: %08x, %016lx)
 *   %r : Nstatus 를 0x%08x 로 출력
 *   %p : 포인터를 0x%016lx 로 출력
 */
#ifndef PRINTK_H
#define PRINTK_H

#include "nyxis.h"

#define FONT_W 16
#define FONT_H 16

Nstatus printk(const char *format, ...);

/*
 * 서식 없이 바이트열을 그대로 콘솔에 출력한다 (사용자 프로그램의 출력용).
 * 보안: 출력 가능한 ASCII 와 \n \r \t 만 통과시키고 나머지 바이트(특히 ESC 등 제어 문자)는 '?' 로 바꾼다.
 * 그렇지 않으면 사용자 프로그램이 시리얼 터미널로 이스케이프 시퀀스를 주입할 수 있다.
 */
void printk_write(const char *buf, usize len);

/*
 * 프레임버퍼 콘솔 초기화. 프레임버퍼가 없거나 값이 이상하면 NinvalidArg 를 반환하며,
 * 이 경우에도 printk 는 시리얼로는 계속 출력한다.
 */
Nstatus printk_init(u32 *framebuffer, u32 pixels_per_scanline, u32 screen_width, u32 screen_height);

#endif /* PRINTK_H */
