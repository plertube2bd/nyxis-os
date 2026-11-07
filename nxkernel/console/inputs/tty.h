/*
 * tty.h - 콘솔 입력 (줄 단위 편집 + 블로킹 읽기)
 *
 * 입력원: PS/2 키보드(IRQ1, inputs_base.c) 와 COM1 시리얼 수신(IRQ4). 둘 다 같은 줄 버퍼로 들어온다
 * (QEMU 의 GTK 창에서 쳐도, -serial stdio 터미널에서 쳐도 같은 stdin 이 된다).
 *
 * 줄 편집 규칙 (IRQ 안에서 처리하고 바로 에코한다):
 *   - 인쇄 가능한 문자/탭: 줄에 추가 (줄 최대 TTY_LINE_MAX-1 자, 넘치면 무시)
 *   - Enter(\r 또는 \n): 줄 끝에 '\n' 을 붙여 "읽을 수 있는 줄" 로 넘기고 대기 중인 읽기를 깨운다
 *   - Backspace(\b, 0x7F): 한 글자 지움.  Ctrl-U: 줄 전체 지움
 *   - Ctrl-D: 줄이 비어 있으면 EOF (다음 읽기가 0 을 반환), 아니면 지금까지의 줄을 개행 없이 넘김
 *   - 그 외 제어 문자는 무시
 *
 * 읽기(tty_read)는 완성된 줄이 생길 때까지 잠든다 (waitq.h). 한 번에 최대 한 줄을 돌려준다.
 */
#ifndef CONSOLE_INPUTS_TTY_H
#define CONSOLE_INPUTS_TTY_H

#include "nyxis.h"

#define TTY_LINE_MAX  256U
#define TTY_READY_MAX 1024U

/* 키보드와 시리얼 수신 인터럽트를 등록하고 켠다 (interrupt_init/pic_remap 이후, sti 이전에 호출) */
void tty_init(void);

/* 입력 문자 하나를 줄 편집기에 넣는다 (입력 드라이버의 IRQ 핸들러가 호출) */
void tty_input_char(char c);

/*
 * 완성된 줄이 있을 때까지 잠든 뒤 최대 len 바이트(최대 한 줄)를 buf 에 복사한다.
 * EOF(Ctrl-D)면 *out_got = 0. idle/부트 스레드는 잠들 수 없으므로 Npermission.
 */
Nstatus tty_read(char *buf, usize len, usize *out_got);

/* 에코 켜기/끄기 (NX_DEVCMD_TTY_ECHO). 꺼도 Enter 의 줄바꿈은 보여 준다 */
void tty_set_echo(bool on);

/* COM1 수신을 stdin 으로 받을지 (dev:/com1 을 사용자 프로세스가 가져가면 끈다) */
void tty_set_serial_rx(bool on);

/* NxPoll 용: 읽을 줄(또는 EOF)이 있는지, 기다릴 대기 큐 */
bool tty_input_ready(void);
struct waitq *tty_waitq(void);

#endif /* CONSOLE_INPUTS_TTY_H */
