/*
 * tty.h - 터미널 (줄 단위 입력 편집 + 블로킹 읽기, 사용자 출력) 과 의사 터미널(pty)
 *
 * 터미널(tty_t)은 두 종류가 있다. 둘 다 같은 줄 편집기/읽기 규칙을 쓴다:
 *   - 콘솔 (tty_console()): 물리 화면/키보드. 입력원은 PS/2 키보드(IRQ1, inputs_base.c)와 COM1 시리얼 수신
 *     (IRQ4). 출력(tty_write)은 터미널 해석기(console/outputs/ansi.h)를 거쳐 콘솔 출력 장치로 간다.
 *   - 의사 터미널 (pty_create): 사용자 프로세스(ntui 의 터미널 창 등)가 "화면과 키보드" 역할을 하는 터미널.
 *     주 끝(master, NX_HANDLE_PTY)에 쓴 바이트가 키 입력이 되고, 종속 끝(slave, 콘솔 핸들)에 쓴 출력과 에코는
 *     걸러지지 않은 바이트 그대로 주 끝에서 읽힌다 (해석은 주 끝을 가진 터미널 에뮬레이터가 한다).
 *
 * 사용자 프로세스의 표준 스트림(NX_HANDLE_CONSOLE: dev:/tty, 처음 받는 0/1/2 번)은 그 프로세스의 제어 터미널
 * (process->ctty, NULL = 콘솔)이다. 제어 터미널은 만든 프로세스에게서 물려받고, 새 프로세스의 0 번 핸들이
 * pty 종속 끝이면 그 pty 가 된다 (syscall.c: sys_process_create / exec_fork).
 *
 * 줄 편집 규칙 (cooked 모드. 콘솔은 IRQ 안에서, pty 는 주 끝 쓰기 안에서 처리하고 바로 에코한다):
 *   - 인쇄 가능한 문자/탭: 줄에 추가 (줄 최대 TTY_LINE_MAX-1 자, 넘치면 무시)
 *   - Enter(\r 또는 \n): 줄 끝에 '\n' 을 붙여 "읽을 수 있는 줄" 로 넘기고 대기 중인 읽기를 깨운다
 *   - Backspace(\b, 0x7F): 한 글자 지움.  Ctrl-U: 줄 전체 지움
 *   - Ctrl-C / Ctrl-\: 줄과 읽지 않은 입력을 버리고 "^C" 를 보인다. 포그라운드 그룹이 있으면 그 그룹에 SIGINT /
 *     SIGQUIT 을 보내고, 없으면 빈 줄을 넘긴다 (NX_DEVCMD_TTY_FOREGROUND 의 설명)
 *   - Ctrl-D: 줄이 비어 있으면 EOF (다음 읽기가 0 을 반환), 아니면 지금까지의 줄을 개행 없이 넘김
 *   - ←/→ 커서 이동, Home/End (Ctrl-A/Ctrl-E) 줄 처음/끝, Delete 커서 자리 지움.
 *     커서가 줄 중간이면 글자는 그 자리에 끼워 넣고, Backspace 는 커서 앞 글자를 지운다.
 *   - ↑/↓ 이전에 입력한 줄 (최근 16 줄, 에코가 꺼진 입력은 기록하지 않음)
 *   - 키 입력원: PS/2 확장 스캔코드는 tty_input_key(), 시리얼/pty 는 VT 이스케이프 시퀀스(ESC [ A 등)를 해석
 *   - 그 외 제어 문자는 무시
 *
 * 읽기(tty_read)는 완성된 줄이 생길 때까지 잠든다 (waitq.h). 한 번에 최대 한 줄을 돌려준다.
 *
 * raw 모드 (NX_DEVCMD_TTY_MODE, nlibc 의 tcsetattr 로 ICANON 을 끄면): 위 규칙 대신 입력 바이트를 에코 없이 바로
 * 읽을 수 있게 하고, 읽기는 있는 만큼 돌려준다. 키보드 편집 키는 VT 시퀀스로 바뀐다 (tty_set_raw).
 *
 * pty 의 수명: 주 끝 참조(핸들)와 종속 끝 참조(핸들 + 그 pty 를 제어 터미널로 가진 프로세스)를 따로 센다.
 *   - 주 끝이 모두 닫히면 끊김(hangup): 종속 끝 읽기는 0(EOF), 쓰기는 Ndisconnected. 그 pty 를 제어 터미널로
 *     가진 프로세스와 포그라운드 그룹에 SIGHUP 을 보낸다.
 *   - 종속 끝이 모두 닫히면 주 끝 읽기는 남은 출력을 다 읽은 뒤 0(EOF), NxPoll 은 HUP.
 *   - 둘 다 0 이 되면 해제한다.
 */
#ifndef CONSOLE_INPUTS_TTY_H
#define CONSOLE_INPUTS_TTY_H

#include "nyxis.h"
#include "kernel/process/waitq.h"

#define TTY_LINE_MAX  256U
#define TTY_READY_MAX 1024U
#define TTY_HIST_MAX  16U
#define PTY_OUT_MAX   4096U     /* 종속 끝 출력 -> 주 끝 읽기 버퍼 (kpage 하나) */

struct pty;
struct nx_winsize;

typedef struct tty {
    struct pty *pty;                /* NULL 이면 콘솔 */

    char *line;                     /* 편집 중인 줄 (TTY_LINE_MAX) */
    u32 line_len;
    u32 pos;                        /* 줄 안의 커서 위치 (0..line_len) */

    char *ready;                    /* 완성되어 읽을 수 있는 바이트 (링, TTY_READY_MAX) */
    u32 ready_head;
    u32 ready_count;
    u32 eof_pending;                /* 아직 소비되지 않은 Ctrl-D(EOF) 수 */
    waitq_t readers;

    bool echo;
    bool raw;
    u32 raw_owner;                  /* raw 를 켠 프로세스 */
    u32 fg_pgid;                    /* 포그라운드 프로세스 그룹 (0 = 없음) */

    char (*hist)[TTY_LINE_MAX];     /* 입력 기록 (TTY_HIST_MAX 줄) */
    u32 hist_len[TTY_HIST_MAX];
    u32 hist_count;
    u32 hist_next;
    u32 hist_browse;

    u32 in_state;                   /* 입력 이스케이프 시퀀스 해석 상태 */
    u32 in_num;
} tty_t;

/* 키보드와 시리얼 수신 인터럽트를 등록하고 켠다 (interrupt_init/pic_remap 이후, sti 이전에 호출) */
void tty_init(void);

/* 콘솔 터미널 */
tty_t *tty_console(void);

/* 입력 문자 하나를 콘솔의 줄 편집기에 넣는다 (입력 드라이버의 IRQ 핸들러, 자체 점검이 호출) */
void tty_input_char(char c);

/* 편집 키 (PS/2 방향키 등). 콘솔로 간다 */
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
 * EOF(Ctrl-D, 끊긴 pty)면 *out_got = 0. idle/부트 스레드는 잠들 수 없으므로 Npermission.
 */
Nstatus tty_read(tty_t *t, char *buf, usize len, usize *out_got);

/*
 * 사용자 출력 (stdout/stderr 쓰기).
 * 콘솔: 바이트는 터미널 해석기(console/outputs/ansi.h)를 거친다. 출력 가능한 ASCII 와 \n \r \t \b, 그리고
 *   허용 목록의 이스케이프 시퀀스(커서 이동, 지우기, 색)만 효과가 있다. 시리얼 터미널로는 검증한 값으로 다시
 *   만든 시퀀스만 나간다: 사용자 프로그램이 시리얼 터미널로 임의의 이스케이프 시퀀스(OSC 등)를 주입하지 못한다.
 * pty: 주 끝이 읽을 버퍼로 그대로. 가득 차면 주 끝이 읽을 때까지 잠든다. 끊겼으면 Ndisconnected.
 * *out_done 은 실제로 넘긴 바이트 수.
 */
Nstatus tty_write(tty_t *t, const char *buf, usize len, usize *out_done);

/*
 * raw 모드 (NX_DEVCMD_TTY_MODE): 줄 편집/에코 없이 입력 바이트를 바로 넘긴다. 편집 키는 VT 시퀀스로 바꾸고
 * Enter 는 '\r'. 켜고 끌 때 아직 읽지 않은 입력은 버린다. owner_pid 는 켠 프로세스 (끝나면 되돌리려고)
 */
void tty_set_raw(tty_t *t, bool on, u32 owner_pid);

/* 프로세스가 끝났다 (process.c). raw 모드를 켠 프로세스면 cooked 로, 색/커서도 기본으로 되돌린다 */
void tty_process_terminated(u32 pid);

/* 창 크기 (NX_DEVCMD_TTY_WINSIZE). 콘솔은 화면 크기, pty 는 주 끝이 정한 크기 */
void tty_get_winsize(tty_t *t, struct nx_winsize *out);

/* 포그라운드 프로세스 그룹 (NX_DEVCMD_TTY_FOREGROUND). 0 = 없음 */
void tty_set_foreground(tty_t *t, u32 pgid);
u32 tty_get_foreground(tty_t *t);

/* 에코 켜기/끄기 (NX_DEVCMD_TTY_ECHO). 꺼도 Enter 의 줄바꿈은 보여 준다 */
void tty_set_echo(tty_t *t, bool on);

/* COM1 수신을 콘솔 stdin 으로 받을지 (dev:/com1 을 사용자 프로세스가 가져가면 끈다) */
void tty_set_serial_rx(bool on);

/* NxPoll 용: 읽을 줄(또는 EOF)이 있는지, 기다릴 대기 큐 */
bool tty_input_ready(tty_t *t);
waitq_t *tty_waitq(tty_t *t);

/* 종속 끝 참조 (콘솔 핸들의 복제/닫기, 제어 터미널). 콘솔(t->pty == NULL)이면 아무 일도 하지 않는다 */
void tty_retain(tty_t *t);
void tty_release(tty_t *t);

/* ---------------- 의사 터미널 ---------------- */

/* 새 pty. 주 끝 참조 1, 종속 끝 참조 1 을 가진 채로 돌려준다. 창 크기 기본 24 x 80 */
Nstatus pty_create(struct pty **out);

/* 종속 끝 터미널 */
tty_t *pty_tty(struct pty *p);

/* 주 끝 참조 */
void pty_master_retain(struct pty *p);
void pty_master_release(struct pty *p);

/* 주 끝 읽기: 종속 끝 출력. 비어 있으면 잠든다. 종속 끝이 모두 닫혔고 비었으면 0 (EOF). 반환: 바이트 수 또는 음수 */
i64 pty_master_read_user(struct pty *p, u64 ubuf, u64 len);

/* 주 끝 쓰기: 키 입력으로 줄 편집기에 넣는다 (잠들지 않는다: 입력 버퍼가 가득 차면 넘치는 것은 버린다) */
i64 pty_master_write_user(struct pty *p, u64 ubuf, u64 len);

/* 창 크기를 정한다 (주 끝, 또는 종속 끝의 NX_DEVCMD_TTY_SET_WINSIZE). 바뀌었으면 포그라운드 그룹에 SIGWINCH */
void pty_set_winsize(struct pty *p, const struct nx_winsize *ws);

/* NxPoll: 주 끝 (IN = 읽을 출력, OUT = 늘, HUP = 종속 끝이 모두 닫혔다) */
u32 pty_master_poll_bits(struct pty *p);
waitq_t *pty_master_waitq(struct pty *p);

/* 종속 끝이 끊겼는지 (주 끝이 모두 닫혔다) */
bool tty_hung_up(tty_t *t);

#endif /* CONSOLE_INPUTS_TTY_H */
