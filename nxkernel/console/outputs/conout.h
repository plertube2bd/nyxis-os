/*
 * conout.h - 콘솔 출력 장치 (프레임버퍼 글자 콘솔 + COM1 시리얼)
 *
 * 화면과 시리얼에 글자를 내보내는 "장치" 계층이다. 이 위에 두 사용자가 있다:
 *   - printk (console/outputs/printk.c): 커널 로그. 서식을 풀어 글자를 여기로 보낸다.
 *   - tty    (console/inputs/tty.c)    : 사용자 프로세스의 stdout/stderr 쓰기와 입력 에코.
 * 둘은 서로를 거치지 않는다: 사용자 프로그램의 출력은 printk 를 지나지 않는다.
 *
 * 출력 싱크(프레임버퍼, 시리얼)는 사용자 프로세스가 장치(dev:/fb0, dev:/com1)를 배타적으로 가져가면
 * 꺼진다 (kernel/dev/device.c). 커널 패닉은 conout_force_all_outputs() 로 모두 다시 켠다.
 * 이 계층은 바이트를 거르지 않는다: 신뢰할 수 없는 바이트(사용자 출력)는 호출자가 걸러서 넘긴다.
 */
#ifndef CONSOLE_OUTPUTS_CONOUT_H
#define CONSOLE_OUTPUTS_CONOUT_H

#include "nyxis.h"

/* 기본 글자색 / 배경색 (0xRRGGBB) */
#define CONOUT_DEFAULT_FG 0xFFFFFFU
#define CONOUT_DEFAULT_BG 0x000000U

/*
 * 프레임버퍼 콘솔 초기화. 프레임버퍼가 없거나 값이 이상하면 NinvalidArg 를 반환하며,
 * 이 경우에도 시리얼로는 계속 출력한다. pixel_format 은 NTBLI_PIXEL_RGB 또는 NTBLI_PIXEL_BGR.
 */
Nstatus conout_init(u32 *framebuffer, u32 pixels_per_scanline, u32 screen_width, u32 screen_height,
                    u32 pixel_format);

/* 켜진 모든 싱크로 한 글자 ('\n' 은 시리얼에 "\r\n", 프레임버퍼는 \n \r \t 를 해석) */
void conout_putc(char c);

/* 마지막 글자 하나를 지운다 (시리얼 "\b \b", 프레임버퍼 커서 후퇴) */
void conout_backspace(void);

/* 커서를 n 칸 왼쪽으로 (지우지 않음). 프레임버퍼는 줄 처음에서 윗줄 끝으로 넘어간다. 시리얼은 '\b' n 개 */
void conout_cursor_back(u32 n);

/*
 * 프레임버퍼 커서(글자 칸 반전 블록) 보이기/숨기기. 기본은 보임.
 * 커서는 conout 이 관리한다: 그리는 함수들이 커서를 지웠다가 끝나면 새 위치에 다시 그린다.
 */
void conout_set_cursor_visible(bool on);

/*
 * 출력 묶음: begin~end 사이에는 글자마다 커서를 지웠다 다시 그리지 않고 end 에서 한 번만 그린다
 * (화면 전체를 다시 그리는 프로그램에서 픽셀 작업이 절반으로 준다). 중첩 가능. 인터럽트(키 에코)가
 * 끼어들어도 커서 상태는 맞다: 그동안 커서가 안 보일 뿐이다.
 */
void conout_batch_begin(void);
void conout_batch_end(void);

/* 출력 싱크 켜기/끄기 */
void conout_set_fb_enabled(bool on);
void conout_set_serial_enabled(bool on);
void conout_force_all_outputs(void);   /* 색도 기본값으로 되돌린다 */

/*
 * 터미널 제어 (console/outputs/ansi.c 가 이스케이프 시퀀스를 해석해 부른다).
 * 위치는 0 부터 세는 글자 칸(행, 열). 프레임버퍼 함수는 프레임버퍼 출력이 꺼져 있으면 아무 일도 하지 않는다.
 * conout_serial_write 는 바이트를 거르지 않고 시리얼로 보낸다: 호출자(ansi.c)가 검증한 시퀀스만 넘긴다.
 */
void conout_serial_write(const char *s, usize len);
void conout_set_color(u32 fg_rgb, u32 bg_rgb);        /* 이후 글자/지우기에 쓸 색 (0xRRGGBB) */
void conout_reset_color(void);
void conout_get_size(u32 *rows, u32 *cols);           /* 프레임버퍼가 없으면 0, 0 */
void conout_get_cursor(u32 *row, u32 *col);
void conout_set_cursor(u32 row, u32 col);             /* 화면 밖이면 가장자리로 */
void conout_erase_display(u32 mode);                  /* 0: 커서~끝, 1: 처음~커서, 2/3: 전체 (커서 그대로) */
void conout_erase_line(u32 mode);                     /* 0: 커서~줄 끝, 1: 줄 처음~커서, 2: 줄 전체 */

#endif /* CONSOLE_OUTPUTS_CONOUT_H */
