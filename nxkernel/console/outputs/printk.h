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
 * 프레임버퍼 콘솔 초기화. 프레임버퍼가 없거나 값이 이상하면 NinvalidArg 를 반환하며,
 * 이 경우에도 printk 는 시리얼로는 계속 출력한다.
 */
Nstatus printk_init(u32 *framebuffer, u32 pixels_per_scanline, u32 screen_width, u32 screen_height);

#endif /* PRINTK_H */
