/*
 * nwp.h - Nyxis Window Protocol: 컴포지터(ntui)와 창 프로그램 사이의 규약 + 클라이언트 라이브러리
 *
 * Wayland 와 같은 구조다:
 *   - 컴포지터만 화면과 키보드를 가진다 (dev:/console). 창 배치, 테두리, 포커스, 단축키, 작업 표시줄은 컴포지터 일.
 *   - 프로그램(클라이언트)은 자기 창의 내용만 그린다: 공유 메모리 버퍼(wl_shm 처럼)에 그리고 commit 하면 컴포지터가
 *     화면에 올린다. 키는 컴포지터가 포커스된 창의 클라이언트에게 이벤트로 보낸다. 크기는 컴포지터가 정한다(configure).
 *   - 클라이언트는 환경 변수 NWP_DISPLAY (WAYLAND_DISPLAY 처럼: "svc:/<사용자>/nwp") 로 컴포지터를 찾는다.
 *     컴포지터가 띄운 프로그램과 그 자식(터미널 안에서 친 명령)은 모두 이 값을 물려받는다.
 *
 * 연결 (docs/NWP.md 에 전체 규약):
 *   클라이언트가 파이프 두 개(위: 클라이언트 -> 컴포지터, 아래: 컴포지터 -> 클라이언트)를 만들고, 컴포지터의 채널에
 *   "nwp up <쿠키>" (위 파이프의 읽기 끝) 과 "nwp down <쿠키>" (아래 파이프의 쓰기 끝) 을 핸들을 붙여 보낸다
 *   (NxChannelSendHandle: Unix 소켓의 fd 넘기기와 같다). 그 뒤의 요청/이벤트는 파이프 위의 메시지다:
 *   struct nwp_hdr + 내용. 버퍼(공유 메모리)도 채널로 "nwp buf <쿠키> <번호>" 에 붙여 보낸다.
 *
 * 지금 버퍼 형식은 글자 칸(NWP_FORMAT_CELLS: struct nwp_cell 격자) 하나다. 픽셀 형식(NWP_FORMAT_XRGB8888)은 번호만
 * 정해 두었다: 그래픽 컴포지터가 생기면 같은 연결/버퍼/configure 규약에 픽셀 버퍼를 붙인다.
 */
#ifndef NWP_H
#define NWP_H

#include <stddef.h>

#define NWP_VERSION 1

/* ================================================================== */
/* 키 (컴포지터가 보내는 값 = 기본 키 | 수정자)                          */
/* ================================================================== */

/*
 * 기본 키: 글자(Shift 반영, 0x20-0x7E), '\r' Enter, '\t' Tab, 0x7F Backspace, NWP_KEY_ESC, 아래 특수 키.
 * Ctrl+글자는 제어 문자(Ctrl+T = 0x14)가 기본 키다 (NWP_MOD_CTRL 은 붙지 않는다. Ctrl+Shift+글자는 NWP_MOD_SHIFT 가 붙는다).
 * Alt/Super 는 비트로. 특수 키와 Tab/Enter 에는 Shift/Ctrl 비트도 붙는다.
 */
enum {
    NWP_KEY_ESC = 27,
    NWP_KEY_UP = 0x100, NWP_KEY_DOWN, NWP_KEY_LEFT, NWP_KEY_RIGHT,
    NWP_KEY_HOME, NWP_KEY_END, NWP_KEY_DEL, NWP_KEY_PGUP, NWP_KEY_PGDN, NWP_KEY_INS,
    NWP_KEY_F1, NWP_KEY_F2, NWP_KEY_F3, NWP_KEY_F4, NWP_KEY_F5, NWP_KEY_F6, NWP_KEY_F7, NWP_KEY_F8,
    NWP_KEY_F9, NWP_KEY_F10, NWP_KEY_F11, NWP_KEY_F12,
    NWP_KEY_SUPER_TAP,                  /* Super 를 눌렀다 뗐다 */
    NWP_KEY_PRINT,                      /* PrintScreen */
    NWP_KEY_MENUKEY,                    /* 메뉴(애플리케이션) 키 */
    NWP_KEY_ALT_UP                      /* Alt 를 뗐다 (컴포지터 안에서만 쓴다) */
};

#define NWP_MOD_SHIFT 0x1000
#define NWP_MOD_CTRL  0x2000
#define NWP_MOD_ALT   0x4000
#define NWP_MOD_SUPER 0x8000
#define NWP_MODS      0xF000
#define NWP_KEY_BASE(k) ((k) & 0x0FFF)
#define NWP_KEY_CTRL(c) ((c) & 0x1F)

/* ================================================================== */
/* 그리기: 글자 칸 + 테마 역할                                           */
/* ================================================================== */

/* 역할 (실제 색은 컴포지터의 테마가 정하고 NWP_EV_THEME 로 알려 준다) */
enum {
    NWP_ROLE_DESKTOP, NWP_ROLE_WINDOW, NWP_ROLE_BORDER,
    NWP_ROLE_TITLE_ACTIVE, NWP_ROLE_TITLE_INACTIVE,
    NWP_ROLE_SELECTION, NWP_ROLE_TASKBAR, NWP_ROLE_ERROR,
    NWP_ROLE_COUNT
};

#define NWP_ATTR_BOLD      0x01
#define NWP_ATTR_REVERSE   0x02
#define NWP_ATTR_UNDERLINE 0x04

/* 상자 그리기 글자 (컴포지터가 화면 모드에서는 선으로, 터미널 위에서는 + - | 로 그린다) */
#define NWP_CH_HLINE 0x2500
#define NWP_CH_VLINE 0x2502
#define NWP_CH_TL    0x250C
#define NWP_CH_TR    0x2510
#define NWP_CH_BL    0x2514
#define NWP_CH_BR    0x2518

/* NWP_FORMAT_CELLS 버퍼의 칸: 8바이트 */
struct nwp_cell {
    unsigned short ch;                  /* 0x20-0x7E, 또는 U+2500-259F. 0 = 공백 */
    unsigned char fg, bg;               /* 256색 번호 */
    unsigned char attr;                 /* NWP_ATTR_* (REVERSE 는 클라이언트가 이미 풀어도 된다) */
    unsigned char pad[3];
};

#define NWP_FORMAT_CELLS    1
#define NWP_FORMAT_XRGB8888 2           /* 아직 없다 (그래픽 컴포지터용으로 번호만) */

/* ================================================================== */
/* 규약: 파이프 위의 메시지                                              */
/* ================================================================== */

struct nwp_hdr {
    unsigned short type;                /* NWP_REQ_* / NWP_EV_* */
    unsigned short len;                 /* 뒤따르는 내용 바이트 (NWP_MSG_MAX 이하) */
    unsigned int surface;               /* 대상 창 (0 = 연결 전체) */
};
#define NWP_MSG_MAX 512

/* 요청 (클라이언트 -> 컴포지터) */
#define NWP_REQ_HELLO        1          /* struct nwp_hello: 연결 뒤 첫 메시지 */
#define NWP_REQ_CREATE       2          /* struct nwp_create: 창 하나 (surface 는 클라이언트가 고른 번호, 1 이상) */
#define NWP_REQ_TITLE        3          /* char[32] */
#define NWP_REQ_ATTACH       4          /* struct nwp_attach: 이 창의 버퍼와 그 안의 크기 */
#define NWP_REQ_COMMIT       5          /* (없음) 버퍼를 화면에 올린다. 컴포지터는 복사한 뒤 NWP_EV_FRAME 을 보낸다 */
#define NWP_REQ_DESTROY      6          /* (없음) 창을 닫는다 */
#define NWP_REQ_STATE        7          /* unsigned int: NWP_STATE_* (최대화/전체 화면/최소화 부탁) */
#define NWP_REQ_INHIBIT      8          /* unsigned int: 1 이면 이 창이 포커스인 동안 단축키 없이 모든 키를 받는다
                                           (키 녹음 같은 것. Wayland 의 keyboard-shortcuts-inhibit) */
#define NWP_REQ_RELOAD       9          /* (없음) 컴포지터가 설정(단축키)을 다시 읽는다 */
#define NWP_REQ_BUFFER_FREE  10         /* unsigned int 버퍼 번호: 컴포지터가 그 버퍼를 놓는다 */
#define NWP_REQ_MOVE         11         /* struct nwp_move: 창(테두리 포함)의 왼쪽 위를 이 칸으로 (보통 배치가 된다) */
#define NWP_REQ_RESIZE       12         /* struct nwp_resize: 안쪽 크기를 이만큼 (0 = 그대로. 보통 배치가 된다).
                                           컴포지터가 창 영역에 맞게 줄일 수 있다: 실제 크기는 CONFIGURE 로 온다 */
#define NWP_REQ_SYNC         13         /* unsigned int 번호: 앞의 요청을 다 처리하면 같은 번호로 NWP_EV_DONE (wl_display.sync).
                                           그 사이에 생긴 CONFIGURE/PLACE 는 DONE 보다 먼저 온다 */

/* 이벤트 (컴포지터 -> 클라이언트) */
#define NWP_EV_WELCOME   64             /* struct nwp_welcome */
#define NWP_EV_THEME     65             /* struct nwp_theme */
#define NWP_EV_CONFIGURE 66             /* struct nwp_configure: 이 크기로 그려 달라 (처음 한 번 + 크기가 바뀔 때마다) */
#define NWP_EV_KEY       67             /* int: 키 (NWP_KEY_* | NWP_MOD_*) */
#define NWP_EV_FOCUS     68             /* unsigned int: 1 포커스 받음, 0 잃음 */
#define NWP_EV_CLOSE     69             /* (없음) 사용자가 창을 닫았다 (Alt+F4): 정리하고 DESTROY 하거나 끝낸다 */
#define NWP_EV_FRAME     70             /* (없음) commit 을 화면에 올렸다: 다음 commit 을 해도 된다 */
#define NWP_EV_SIGNAL    71             /* unsigned int: 단축키의 signal 문장 (터미널은 자기 작업에 보낸다) */
#define NWP_EV_ERROR     72             /* char[]: 규약 위반. 컴포지터가 곧 연결을 끊는다 */
#define NWP_EV_PLACE     73             /* struct nwp_place: 창의 자리 (처음 한 번 + 옮겨질 때마다) */
#define NWP_EV_DONE      74             /* unsigned int: NWP_REQ_SYNC 의 답 */

#define NWP_STATE_NORMAL     0
#define NWP_STATE_MAXIMIZED  1
#define NWP_STATE_FULLSCREEN 2
#define NWP_STATE_MINIMIZED  3

struct nwp_hello {
    unsigned int version;               /* NWP_VERSION */
    unsigned int pid;
    char app_id[16];                    /* 프로그램 종류 ("nterminal", "nfiles" ...): 단축키의 @앱, 작업 관리 */
};

struct nwp_create {
    unsigned short cols, rows;          /* 바라는 크기 (0 = 컴포지터가 정한다) */
    unsigned int state;                 /* NWP_STATE_* 처음 상태 */
    char title[32];
};

struct nwp_attach {
    unsigned int buffer;                /* "nwp buf" 로 보낸 번호 */
    unsigned short cols, rows;          /* 버퍼 안의 격자 크기 (칸 = cols * rows, 줄 순서) */
    unsigned int format;                /* NWP_FORMAT_CELLS */
};

#define NWP_WELCOME_DISPLAY 0x1         /* 컴포지터가 화면을 직접 그린다 (아니면 터미널 위에서: Super 키가 없다) */

struct nwp_welcome {
    unsigned int version;
    unsigned int flags;                 /* NWP_WELCOME_* */
    unsigned short screen_cols, screen_rows;   /* 창이 가질 수 있는 가장 큰 크기 (버퍼를 이만큼 잡으면 다시 만들 일이 없다) */
};

struct nwp_theme {
    unsigned char role[NWP_ROLE_COUNT][4];   /* fg, bg, attr, 0 */
    char name[16];
};

struct nwp_configure {
    unsigned short cols, rows;
    unsigned int state;                 /* NWP_STATE_* */
};

struct nwp_move {
    short x, y;                         /* 화면 칸 (0, 0 = 왼쪽 위) */
};

struct nwp_resize {
    unsigned short cols, rows;          /* 안쪽 크기 (0 = 그대로) */
};

struct nwp_place {
    short x, y;                         /* 창(테두리 포함)의 왼쪽 위. 전체 화면이면 0, 0 */
    unsigned short area_cols, area_rows;   /* 창이 놓일 수 있는 영역 (화면에서 작업 표시줄을 뺀 것, 테두리 포함) */
};

/* ================================================================== */
/* 클라이언트 라이브러리 (창 하나)                                       */
/* ================================================================== */

struct nwp_window;

/*
 * 컴포지터에 연결해 창을 연다. 첫 configure 를 받을 때까지(최대 3초) 기다린다. 실패하면 NULL + errno
 * (ENOENT: NWP_DISPLAY 가 없거나 컴포지터가 없다).
 *   app_id  프로그램 종류 ("nterminal" 처럼, 15자까지)
 *   cols/rows 바라는 크기 (0 = 컴포지터가 정한다)   state NWP_STATE_*
 */
struct nwp_window *nwp_open(const char *app_id, const char *title, int cols, int rows, unsigned state);
void nwp_close(struct nwp_window *w);

/* poll 로 기다릴 fd (이벤트가 오면 POLLIN) */
int nwp_fd(const struct nwp_window *w);

/* 이벤트 하나 */
#define NWP_EVENT_KEY    1              /* key */
#define NWP_EVENT_RESIZE 2              /* cols/rows 가 바뀌었다: 다시 그리고 commit */
#define NWP_EVENT_FOCUS  3              /* focused */
#define NWP_EVENT_CLOSE  4              /* 사용자가 닫았다 */
#define NWP_EVENT_THEME  5              /* 색이 바뀌었다: 다시 그리고 commit */
#define NWP_EVENT_SIGNAL 6              /* sig */
struct nwp_event {
    int type;
    int key;
    int cols, rows;
    int focused;
    int sig;
};

/* 이벤트를 하나 받는다 (timeout_ms: 0 기다리지 않음, 음수 끝없이). 1 받음, 0 시간 초과, -1 연결이 끊김 (컴포지터가 없어졌다) */
int nwp_next_event(struct nwp_window *w, struct nwp_event *ev, int timeout_ms);

int nwp_cols(const struct nwp_window *w);
int nwp_rows(const struct nwp_window *w);
int nwp_focused(const struct nwp_window *w);
int nwp_display_mode(const struct nwp_window *w);   /* 컴포지터가 화면을 직접 그린다 (Super 키가 있다) */

/* 그리기 (클라이언트 자신의 칸에. nwp_commit 이 화면으로 보낸다). 창 밖은 잘린다 */
void nwp_style(const struct nwp_window *w, int role, int *fg, int *bg, unsigned *attr);
void nwp_put(struct nwp_window *w, int x, int y, int ch, int fg, int bg, unsigned attr);
void nwp_put_role(struct nwp_window *w, int x, int y, int ch, int role);
int nwp_text(struct nwp_window *w, int x, int y, const char *s, int role);       /* 쓴 뒤의 x */
void nwp_fill(struct nwp_window *w, int x, int y, int cw, int ch, int c, int role);
void nwp_box(struct nwp_window *w, int x, int y, int bw, int bh, int role);
void nwp_clear(struct nwp_window *w, int role);
/* 화면에 올린다. 앞의 commit 이 아직 올라가지 않았으면 그 뒤에 올린다 (그리기를 막지 않는다). 성공 0 */
int nwp_commit(struct nwp_window *w);

void nwp_set_title(struct nwp_window *w, const char *title);
void nwp_set_state(struct nwp_window *w, unsigned state);
/* 창 옮기기 / 크기 바꾸기 부탁 (컴포지터가 정한다: 결과는 RESIZE 이벤트와 nwp_x/nwp_y) */
void nwp_move(struct nwp_window *w, int x, int y);
void nwp_resize(struct nwp_window *w, int cols, int rows);
int nwp_state(const struct nwp_window *w);         /* NWP_STATE_* (마지막 CONFIGURE) */
int nwp_x(const struct nwp_window *w);             /* 창의 왼쪽 위 (마지막 PLACE, 모르면 -1) */
int nwp_y(const struct nwp_window *w);
int nwp_area_cols(const struct nwp_window *w);     /* 창 영역 (테두리 포함, 모르면 0) */
int nwp_area_rows(const struct nwp_window *w);
/* 앞의 요청(옮기기, 크기, 상태 ...)을 컴포지터가 처리하고 그 결과(CONFIGURE/PLACE)가 올 때까지 기다린다 (최대 timeout_ms).
 * 그동안 온 키 같은 이벤트는 쌓아 두었다가 nwp_next_event 가 돌려준다: 이것을 부른 뒤에는 nwp_next_event(w, &ev, 0) 이
 * 0 을 돌려줄 때까지 비운다 (fd 를 poll 해도 이미 읽은 것은 깨우지 않는다). 성공 0, 시간 초과/끊김 -1 */
int nwp_roundtrip(struct nwp_window *w, int timeout_ms);
void nwp_inhibit_shortcuts(struct nwp_window *w, int on);
void nwp_reload_config(struct nwp_window *w);

/* ---- 한 줄 입력 (여러 OS 의 편집 키: <- -> Home End Ctrl-A/E, Ctrl+<- ->, Ctrl+Backspace/Ctrl-W, Ctrl-U/K, Delete) ---- */
struct nwp_lineedit {
    char *buf;
    int cap;                            /* buf 크기 (NUL 포함) */
    int len;
    int pos;
};
void nwp_le_init(struct nwp_lineedit *le, char *buf, int cap);
void nwp_le_set(struct nwp_lineedit *le, const char *s);
int nwp_le_key(struct nwp_lineedit *le, int key);  /* 편집 키면 처리하고 1 */
/* [x, x+width) 에 그린다 (커서가 보이도록 민다). secret 이면 '*' */
void nwp_le_draw(struct nwp_window *w, const struct nwp_lineedit *le, int x, int y, int width, int secret, int cursor);

/* 컴포지터 안에서 돈다 (NWP_DISPLAY 가 있다) */
int nwp_available(void);

/*
 * 짧은 이름 (선택): #define NWP_SHORT_NAMES 를 하고 포함하면 KEY_* / ROLE_* 이름을 쓸 수 있다
 * (ntui 에서 떼어 낸 창 프로그램들이 그대로 쓴다)
 */
#ifdef NWP_SHORT_NAMES
#define KEY_ESC       NWP_KEY_ESC
#define KEY_UP        NWP_KEY_UP
#define KEY_DOWN      NWP_KEY_DOWN
#define KEY_LEFT      NWP_KEY_LEFT
#define KEY_RIGHT     NWP_KEY_RIGHT
#define KEY_HOME      NWP_KEY_HOME
#define KEY_END       NWP_KEY_END
#define KEY_DEL       NWP_KEY_DEL
#define KEY_PGUP      NWP_KEY_PGUP
#define KEY_PGDN      NWP_KEY_PGDN
#define KEY_INS       NWP_KEY_INS
#define KEY_ALT_UP    NWP_KEY_ALT_UP
#define KEY_MOD_SHIFT NWP_MOD_SHIFT
#define KEY_MOD_CTRL  NWP_MOD_CTRL
#define KEY_MOD_ALT   NWP_MOD_ALT
#define KEY_MOD_SUPER NWP_MOD_SUPER
#define KEY_BASE(k)   NWP_KEY_BASE(k)
#define KEY_CTRL(c)   NWP_KEY_CTRL(c)
#define ROLE_DESKTOP        NWP_ROLE_DESKTOP
#define ROLE_WINDOW         NWP_ROLE_WINDOW
#define ROLE_BORDER         NWP_ROLE_BORDER
#define ROLE_TITLE_ACTIVE   NWP_ROLE_TITLE_ACTIVE
#define ROLE_TITLE_INACTIVE NWP_ROLE_TITLE_INACTIVE
#define ROLE_SELECTION      NWP_ROLE_SELECTION
#define ROLE_TASKBAR        NWP_ROLE_TASKBAR
#define ROLE_ERROR          NWP_ROLE_ERROR
#endif

#endif /* NWP_H */
