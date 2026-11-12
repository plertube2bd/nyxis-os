/*
 * nx64.h - nx64api: Nyxis OS 용 C API (TUI 앱 + 비 POSIX 기능)
 *
 * 링크 옵션 없이 쓴다 (nlibc 에 들어 있다): 호스트에서는 out/nlibc/bin/nlcc, Nyxis 안에서는 nlcc.
 *
 *   #include <nx64.h>
 *
 *   static void draw(void *ctx) {
 *       nx64_tui_clear(NX64_DEFAULT);
 *       nx64_tui_box(2, 1, 30, 5, "hello", NX64_FRAME);
 *       nx64_tui_text(4, 3, "q: quit", NX64_DEFAULT);
 *   }
 *   static int key(void *ctx, int k) { return k != 'q'; }   (0 을 돌려주면 끝)
 *
 *   int main(void) {
 *       struct nx64_app app = { draw, key, NULL, 0 };
 *       return nx64_tui_run(&app, NULL);
 *   }
 *
 * 두 부분:
 *   1. TUI   전체 화면 텍스트 UI. 셀 버퍼에 그리고 nx64_tui_flush 가 바뀐 칸만 내보낸다 (깜빡임 없음).
 *            ntui(Nyxis OS 데스크톱)에서는 실행 상자(Super+R)로 터미널 창에서, 셸에서는 그냥 실행한다.
 *   2. 시스템 POSIX 에 없는 것: info:/ 값, 메모리, 신원, 앱 설정(app:/<앱>.run/<키>), svc:/ 서비스 채널,
 *            전원 끄기 요청.
 *   3. 화면  dev:/console: 프레임버퍼 + 키 이벤트 (콘솔 세션 프로세스와 root 만. ntui 가 쓴다).
 *            의사 터미널은 <pty.h> 의 openpty.
 * 오류: 실패하면 -1 (또는 NULL) 을 돌려주고 errno 를 정한다 (POSIX 와 같다: strerror(errno)).
 * 모든 함수는 C89 로 쓸 수 있다.
 */
#ifndef NX64_H
#define NX64_H

#include <stddef.h>
#include <nyxapp.h>

#define NX64_API_VERSION 1

/* ================================================================== */
/* 1. TUI                                                              */
/* ================================================================== */

/* 글자 스타일: 256색 번호(fg, bg) + 속성 */
typedef struct nx64_style {
    unsigned char fg;
    unsigned char bg;
    unsigned char attr;
} nx64_style;

#define NX64_BOLD      0x01
#define NX64_UNDERLINE 0x02
#define NX64_REVERSE   0x04

/* 기본 16색 (256색 번호와 같다) */
enum {
    NX64_BLACK, NX64_RED, NX64_GREEN, NX64_YELLOW, NX64_BLUE, NX64_MAGENTA, NX64_CYAN, NX64_WHITE,
    NX64_GRAY, NX64_BRIGHT_RED, NX64_BRIGHT_GREEN, NX64_BRIGHT_YELLOW, NX64_BRIGHT_BLUE,
    NX64_BRIGHT_MAGENTA, NX64_BRIGHT_CYAN, NX64_BRIGHT_WHITE
};

/* 스타일 만들기 (C89 에는 복합 리터럴이 없어 함수로) */
nx64_style nx64_style_make(int fg, int bg, int attr);

/* Nyxis OS 데스크톱(nyx 테마)과 어울리는 기본 스타일 */
extern const nx64_style NX64_DEFAULT;     /* 바탕 글자 */
extern const nx64_style NX64_FRAME;       /* 테두리, 제목 */
extern const nx64_style NX64_ACCENT;      /* 강조 */
extern const nx64_style NX64_SELECTED;    /* 고른 항목 */
extern const nx64_style NX64_DIM;         /* 흐린 글자 */

/* 키 (일반 글자는 0..255 그대로. Enter = '\r', Backspace = 127 또는 8) */
enum {
    NX64_KEY_ESC = 27,
    NX64_KEY_UP = 0x100, NX64_KEY_DOWN, NX64_KEY_LEFT, NX64_KEY_RIGHT,
    NX64_KEY_HOME, NX64_KEY_END, NX64_KEY_DEL, NX64_KEY_PGUP, NX64_KEY_PGDN
};
#define NX64_KEY_CTRL(c) ((c) & 0x1F)    /* NX64_KEY_CTRL('q') */
#define NX64_KEY_NONE    (-2)            /* nx64_tui_key: 시간 안에 키가 없었다 */

/* 화면을 넘겨받는다 (raw 모드, 커서 숨김, 셀 버퍼). 성공 0, 실패 -1. 끝나면 nx64_tui_end */
int nx64_tui_begin(void);
/* 화면을 돌려준다 (색/커서를 되돌리고 지운다). 프로그램이 끝날 때도 자동으로 불린다 */
void nx64_tui_end(void);
int nx64_tui_cols(void);
int nx64_tui_rows(void);

/* 그리기 (셀 버퍼에만: nx64_tui_flush 가 화면에 낸다). 화면 밖은 잘리고, 출력할 수 없는 글자는 '?' */
void nx64_tui_clear(nx64_style s);
void nx64_tui_put(int x, int y, int ch, nx64_style s);
int  nx64_tui_text(int x, int y, const char *text, nx64_style s);          /* 쓴 뒤의 x */
int  nx64_tui_textf(int x, int y, nx64_style s, const char *fmt, ...);     /* printf 형식 (한 줄, 256자까지) */
void nx64_tui_fill(int x, int y, int w, int h, int ch, nx64_style s);
/* 테두리 (+ - |, w/h 는 테두리 포함). title 이 있으면 윗변에 " title " 로. 안쪽은 지우지 않는다 */
void nx64_tui_box(int x, int y, int w, int h, const char *title, nx64_style s);
/* 막대 그래프: [x, x+w) 를 value/max 만큼 on 으로, 나머지를 off 로 */
void nx64_tui_bar(int x, int y, int w, unsigned long value, unsigned long max, nx64_style on, nx64_style off);
/* 바뀐 칸만 화면으로 */
void nx64_tui_flush(void);
/* 다음 flush 가 모든 칸을 다시 보내게 (다른 출력이 화면을 덮었을 때) */
void nx64_tui_invalidate(void);

/* 키 하나를 최대 timeout_ms 기다린다 (100ms 단위, < 0 이면 끝없이). 없으면 NX64_KEY_NONE, 입력이 끝나면 -1 */
int nx64_tui_key(int timeout_ms);

/* 이벤트 루프 */
struct nx64_app {
    void (*draw)(void *ctx);            /* 화면을 그린다 (flush 는 루프가 한다) */
    int  (*key)(void *ctx, int key);    /* 키 하나. 0 을 돌려주면 루프가 끝난다 */
    int  (*tick)(void *ctx);            /* tick_ms 마다 (NULL 가능). 다시 그려야 하면 1 */
    int tick_ms;                        /* 0 이면 tick 없이 키만 기다린다 */
};
/* begin -> (draw, flush, 키/tick 기다리기) 반복 -> end. 성공 0, 화면을 못 얻으면 -1 */
int nx64_tui_run(const struct nx64_app *app, void *ctx);

/* ================================================================== */
/* 2. 시스템 (비 POSIX)                                                 */
/* ================================================================== */

/* info:/<name> 의 값 (한 줄, 끝의 개행은 뗀다). 예: "hostname", "version", "timezone". 값의 길이 또는 -1 */
long nx64_info(const char *name, char *buf, size_t cap);
/* info:/<name> 을 숫자로 (예: "uptime_ms", "memory/free_kib"). 성공 0 */
int nx64_info_ulong(const char *name, unsigned long *out);

struct nx64_memory {
    unsigned long ram_kib;              /* 물리 RAM 전체 */
    unsigned long free_kib;             /* 남은 것 */
    unsigned long used_kib;             /* ram - free (커널, initrd, tmp:/ 포함) */
    unsigned long reserved_kib;         /* 할당기 밖의 RAM (커널 이미지, initrd, 펌웨어) */
    unsigned long managed_kib;          /* 할당기(pfa)가 관리하는 RAM (= ram - reserved) */
    unsigned long alloc_kib;            /* managed - free: 할당기에서 나간 것 (옛 Nyxis 방식의 "사용") */
};
int nx64_memory(struct nx64_memory *out);
unsigned long nx64_uptime_ms(void);     /* 모르면 0 */

/* 나 (커널이 아는 신원: 토큰으로 로그인한 사용자) */
struct nx64_identity {
    unsigned long uid;
    unsigned long gid;
    char name[56];
};
int nx64_whoami(struct nx64_identity *out);

/* 앱 설정 (nyxapp.h 와 같다): 사용자 값 roi:/<uid>/app/<앱>/<키> -> 기본값 app:/<앱>.run/<키>
 *   get    값 (끝 개행 하나는 뗀다). 길이, 없으면 -1/ENOENT
 *   set    사용자 값으로 저장 (상위 디렉터리는 0700 으로 만든다)
 *   reset  사용자 값을 지운다 (기본값이 다시 보인다)
 *   path   설정 파일 경로 (여러 줄 설정을 직접 읽을 때: nysh 의 rc). where 는 NX64_SETTING_DEFAULT / _USER */
#define NX64_SETTING_DEFAULT NYX_SETTING_DEFAULT
#define NX64_SETTING_USER    NYX_SETTING_USER
long nx64_setting_get(const char *app, const char *key, char *buf, size_t cap);
int  nx64_setting_set(const char *app, const char *key, const char *value);
int  nx64_setting_reset(const char *app, const char *key);
int  nx64_setting_path(int where, const char *app, const char *key, char *buf, size_t cap);

/*
 * 서비스 (svc:/ 채널: 커널이 메시지마다 보낸 쪽 pid/uid 를 붙인다 - 사칭할 수 없다)
 *   name 이 "svc:/" 로 시작하면 그대로, 아니면 "svc:/<내 사용자 이름>/<name>".
 *   nx64_service_create: 받는 쪽. 핸들(닫으면 이름이 사라진다: close()) 또는 -1. public 이면 누구나 보낼 수 있다
 *   nx64_service_receive: 메시지 하나 (timeout_ms < 0 이면 끝없이). 길이, 시간 초과면 -1/ETIMEDOUT
 *   nx64_service_send: 보내는 쪽 (열고 보내고 닫는다). 성공 0
 */
struct nx64_sender {
    unsigned long pid;
    unsigned long uid;
    unsigned long gid;
};
long nx64_service_create(const char *name, int public_);
long nx64_service_receive(long h, void *buf, size_t cap, struct nx64_sender *from, int timeout_ms);
int  nx64_service_send(const char *name, const void *buf, size_t len);

/* init 에게 전원을 꺼 달라고 요청한다 (svc:/sys/init). 성공 0: 곧 꺼진다 */
int nx64_request_poweroff(void);

/* ---------------- 프로세스 (ntop) ---------------- */

struct nx64_proc {
    unsigned long pid, ppid, pgid, uid;
    int state;                          /* NX64_PROC_READY / RUNNING / BLOCKED */
    unsigned int flags;                 /* NX64_PROC_USER / SESSION / PTY */
    unsigned long cpu_ticks;            /* CPU 를 쓴 타이머 틱 (nx64_timer_hz 로 나누면 초) */
    unsigned long start_tick;           /* 만든 시각 (부팅 후 틱) */
    unsigned long mem_kib;              /* 사용자 메모리 */
    char name[32];                      /* 실행 파일 이름 (커널이 정한다: argv[0] 이 아니다) */
    char user[32];
};
#define NX64_PROC_READY   0
#define NX64_PROC_RUNNING 1
#define NX64_PROC_BLOCKED 2
#define NX64_PROC_USER    0x1           /* 사용자 프로세스 (아니면 커널 스레드: idle 등) */
#define NX64_PROC_SESSION 0x2           /* 콘솔 세션 (로그인 셸) */
#define NX64_PROC_PTY     0x4           /* 터미널이 의사 터미널 (ntui 의 창 등) */
#define NX64_PROC_CRITICAL 0x8          /* 보호 프로세스 (init, login): 부모 말고는 root 도 끝낼 수 없다 */

/* 살아 있는 프로세스를 최대 max 개 out 에 (pid 순). 전체 수를 돌려준다 (max 보다 클 수 있다), 오류 -1 */
int nx64_processes(struct nx64_proc *out, int max);
/* pid 로 신호 (SIGTERM 등). 자기 uid 의 프로세스만 (root 는 모두). 성공 0, 실패 -1 (EPERM, ESRCH) */
int nx64_signal_pid(unsigned long pid, int sig);
/* 이름이 name 인 다른 프로세스(자기 자신 제외)의 pid, 없으면 0 */
unsigned long nx64_find_process(const char *name);
unsigned long nx64_timer_hz(void);
/* 지금 사용자의 비밀번호가 맞는지 (화면 잠금). 맞으면 0, 틀리면 -1 + EACCES (커널이 1초 늦춘다) */
int nx64_check_password(const char *password);

/* ================================================================== */
/* 3. 화면 (dev:/console)                                              */
/* ================================================================== */

/*
 * 화면 전체와 키보드를 가져온다. 로그인 셸(로그인이 콘솔 세션으로 띄운 그 프로세스)이나 root 만 된다
 * (그 밖에는 -1 + EACCES, 다른 프로세스가 쓰는 중이면 EBUSY, 프레임버퍼가 없으면 ENOENT).
 * 그동안 커널 콘솔은 화면에 그리지 않고, PS/2 키는 stdin 대신 nx64_display_keys 로 온다 (시리얼 입력은 그대로 stdin).
 * nx64_display_close 나 프로세스가 끝나면 화면을 지우고 커널 콘솔로 돌려준다.
 */
struct nx64_display {
    int fd;                             /* NxPoll 로 키를 기다릴 때 (poll 의 POLLIN) */
    unsigned int *fb;                   /* 픽셀 (32비트). y 줄은 fb + y * stride */
    int width, height;                  /* 픽셀 */
    int stride;                         /* 한 줄의 픽셀 수 */
    int bgr;                            /* 1 이면 픽셀 = 0x00BBGGRR, 0 이면 0x00RRGGBB (nx64_display_rgb 가 맞춘다) */
    int cell_w, cell_h;                 /* 커널 콘솔 글꼴의 글자 칸 크기 */
    unsigned char *font;                /* 글꼴: 글자 font_first + i 의 알파 = font + i * cell_w * cell_h (malloc) */
    int font_first, font_count;
};

int nx64_display_open(struct nx64_display *d);
void nx64_display_close(struct nx64_display *d);

/* 0xRRGGBB -> 이 화면의 픽셀 값 */
unsigned int nx64_display_rgb(const struct nx64_display *d, unsigned int rgb);

/* 키 이벤트 (struct nx_keyevent 와 같다) */
struct nx64_keyevent {
    unsigned short code;                /* NX64_KC_*: PS/2 세트 1 누름 코드, E0 확장 키는 + 0x100 */
    unsigned short mods;                /* NX64_KMOD_* (이 이벤트를 반영한 뒤) */
    unsigned char pressed;              /* 1 누름(자동 반복 포함), 0 뗌 */
    unsigned char ch;                   /* Shift/Caps 를 반영한 ASCII, 글자가 아니면 0 */
    unsigned short reserved;
};

#define NX64_KMOD_SHIFT 0x01
#define NX64_KMOD_CTRL  0x02
#define NX64_KMOD_ALT   0x04
#define NX64_KMOD_SUPER 0x08
#define NX64_KMOD_ALTGR 0x10
#define NX64_KMOD_CAPS  0x20

#define NX64_KC_ESC       0x001
#define NX64_KC_BACKSPACE 0x00E
#define NX64_KC_TAB       0x00F
#define NX64_KC_ENTER     0x01C
#define NX64_KC_F1        0x03B          /* F1..F10 = 0x3B..0x44 */
#define NX64_KC_F10       0x044
#define NX64_KC_F11       0x057
#define NX64_KC_F12       0x058
#define NX64_KC_KP_ENTER  0x11C
#define NX64_KC_HOME      0x147
#define NX64_KC_UP        0x148
#define NX64_KC_PGUP      0x149
#define NX64_KC_LEFT      0x14B
#define NX64_KC_RIGHT     0x14D
#define NX64_KC_END       0x14F
#define NX64_KC_DOWN      0x150
#define NX64_KC_PGDN      0x151
#define NX64_KC_INSERT    0x152
#define NX64_KC_DELETE    0x153
#define NX64_KC_LSUPER    0x15B
#define NX64_KC_RSUPER    0x15C
#define NX64_KC_MENU      0x15D          /* 메뉴(애플리케이션) 키 */
#define NX64_KC_PRTSC     0x137          /* PrintScreen */
#define NX64_KC_LALT      0x038
#define NX64_KC_RALT      0x138

/* 쌓인 키 이벤트를 최대 max 개 읽는다. timeout_ms: 0 이면 기다리지 않음, 음수면 끝없이. 읽은 개수, 오류 -1 */
int nx64_display_keys(struct nx64_display *d, struct nx64_keyevent *ev, int max, int timeout_ms);

#endif /* NX64_H */
