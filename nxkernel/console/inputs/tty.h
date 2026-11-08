/*
 * tty.h - 콘솔 터미널 (줄 단위 입력 편집 + 블로킹 읽기, 사용자 출력)
 *
 * 사용자 프로세스의 표준 스트림(NX_HANDLE_CONSOLE: dev:/tty, 처음 받는 0/1/2 번)이 이 터미널이다.
 * 출력(tty_write)은 콘솔 출력 장치(console/outputs/conout.h)로 바로 간다: 커널 로그(printk)를 거치지 않는다.
 *
 * 입력원: PS/2 키보드(IRQ1, inputs_base.c) 와 COM1 시리얼 수신(IRQ4). 둘 다 같은 줄 버퍼로 들어온다
 * (QEMU 의 GTK 창에서 쳐도, -serial stdio 터미널에서 쳐도 같은 stdin 이 된다).
 *
 * 줄 편집 규칙 (IRQ 안에서 처리하고 바로 에코한다):
 *   - 인쇄 가능한 문자/탭: 줄에 추가 (줄 최대 TTY_LINE_MAX-1 자, 넘치면 무시)
 *   - Enter(\r 또는 \n): 줄 끝에 '\n' 을 붙여 "읽을 수 있는 줄" 로 넘기고 대기 중인 읽기를 깨운다
 *   - Backspace(\b, 0x7F): 한 글자 지움.  Ctrl-U: 줄 전체 지움
 *   - Ctrl-C / Ctrl-\: 줄과 읽지 않은 입력을 버리고 "^C" 를 보인다. 포그라운드 그룹이 있으면 그 그룹에 SIGINT /
 *     SIGQUIT 을 보내고, 없으면 빈 줄을 넘긴다 (NX_DEVCMD_TTY_FOREGROUND 의 설명)
 *   - Ctrl-D: 줄이 비어 있으면 EOF (다음 읽기가 0 을 반환), 아니면 지금까지의 줄을 개행 없이 넘김
 *   - ←/→ 커서 이동, Home/End (Ctrl-A/Ctrl-E) 줄 처음/끝, Delete 커서 자리 지움.
 *     커서가 줄 중간이면 글자는 그 자리에 끼워 넣고, Backspace 는 커서 앞 글자를 지운다.
 *   - ↑/↓ 이전에 입력한 줄 (최근 16 줄, 에코가 꺼진 입력은 기록하지 않음)
 *   - 키 입력원: PS/2 확장 스캔코드는 tty_input_key(), 시리얼은 VT 이스케이프 시퀀스(ESC [ A 등)를 여기서 해석
 *   - 그 외 제어 문자는 무시
 *
 * 읽기(tty_read)는 완성된 줄이 생길 때까지 잠든다 (waitq.h). 한 번에 최대 한 줄을 돌려준다.
 *
 * raw 모드 (NX_DEVCMD_TTY_MODE, nlibc 의 tcsetattr 로 ICANON 을 끄면): 위 규칙 대신 입력 바이트를 에코 없이 바로
 * 읽을 수 있게 하고, 읽기는 있는 만큼 돌려준다. 키보드 편집 키는 VT 시퀀스로 바뀐다 (tty_set_raw).
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

/* 편집 키 (PS/2 방향키 등) */
#define TTY_KEY_UP     1U
#define TTY_KEY_DOWN   2U
#define TTY_KEY_LEFT   3U
#define TTY_KEY_RIGHT  4U
#define TTY_KEY_HOME   5U
#define TTY_KEY_END    6U
#define TTY_KEY_DELETE 7U
#define TTY_KEY_PGUP   8U
#define TTY_KEY_PGDN   9U
void tty_input_key(u32 key);

/*
 * 완성된 줄이 있을 때까지 잠든 뒤 최대 len 바이트(최대 한 줄)를 buf 에 복사한다.
 * EOF(Ctrl-D)면 *out_got = 0. idle/부트 스레드는 잠들 수 없으므로 Npermission.
 */
Nstatus tty_read(char *buf, usize len, usize *out_got);

/*
 * 사용자 출력 (stdout/stderr 쓰기). 바이트는 터미널 해석기(console/outputs/ansi.h)를 거친다:
 * 출력 가능한 ASCII 와 \n \r \t \b, 그리고 허용 목록의 이스케이프 시퀀스(커서 이동, 지우기, 색)만 효과가 있다.
 * 보안: 시리얼 터미널로는 검증한 값으로 다시 만든 시퀀스만 나가고, 그 밖의 제어 바이트는 '?' 로 바뀌거나 버려진다:
 * 사용자 프로그램이 시리얼 터미널로 임의의 이스케이프 시퀀스(OSC 등)를 주입하지 못한다.
 */
void tty_write(const char *buf, usize len);

/*
 * raw 모드 (NX_DEVCMD_TTY_MODE): 줄 편집/에코 없이 입력 바이트를 바로 넘긴다. 편집 키는 VT 시퀀스로 바꾸고
 * Enter 는 '\r'. 켜고 끌 때 아직 읽지 않은 입력은 버린다. owner_pid 는 켠 프로세스 (끝나면 되돌리려고)
 */
void tty_set_raw(bool on, u32 owner_pid);

/* 프로세스가 끝났다 (process.c). raw 모드를 켠 프로세스면 cooked 로, 색/커서도 기본으로 되돌린다 */
void tty_process_terminated(u32 pid);

/* 창 크기 (NX_DEVCMD_TTY_WINSIZE) */
struct nx_winsize;
void tty_get_winsize(struct nx_winsize *out);

/* 포그라운드 프로세스 그룹 (NX_DEVCMD_TTY_FOREGROUND). 0 = 없음 */
void tty_set_foreground(u32 pgid);
u32 tty_get_foreground(void);

/* 에코 켜기/끄기 (NX_DEVCMD_TTY_ECHO). 꺼도 Enter 의 줄바꿈은 보여 준다 */
void tty_set_echo(bool on);

/* COM1 수신을 stdin 으로 받을지 (dev:/com1 을 사용자 프로세스가 가져가면 끈다) */
void tty_set_serial_rx(bool on);

/* NxPoll 용: 읽을 줄(또는 EOF)이 있는지, 기다릴 대기 큐 */
bool tty_input_ready(void);
struct waitq *tty_waitq(void);

#endif /* CONSOLE_INPUTS_TTY_H */
