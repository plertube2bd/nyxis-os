/*
 * ntui.h - ntui 공용 선언
 *
 * ntui 는 두 가지로 돈다 (screen.c / term.c 가 고른다):
 *   - 화면 모드 (기본): 로그인 셸(콘솔 세션)로 시작하면 dev:/console 을 가져와 프레임버퍼에 직접 그리고, 키보드의
 *     키 이벤트를 받는다 (Shift/Ctrl/Alt/Super 구분). 커널 콘솔은 그동안 화면에 그리지 않는다.
 *   - 터미널 모드: 화면을 가져올 수 없으면(셸에서 띄움, 프레임버퍼 없음) 지금 터미널 위에서 ANSI 로 그린다.
 * 어느 모드든 창 안의 터미널은 ntui 의 터미널 에뮬레이터(vt.c)이고, 명령은 의사 터미널(openpty)에서 돈다.
 *
 *   term.c   입력: 키 (화면 모드의 키 이벤트 / 터미널 모드의 바이트 해석) -> KEY_* + 수정자
 *   sys.c    세션 정보: 사용자, 시간대 (init)
 *   theme.c  색 테마 (역할 -> 색)
 *   screen.c 화면: 셀 버퍼, 바뀐 칸만 그리기 (프레임버퍼 / ANSI)
 *   vt.c     터미널 에뮬레이터 (VT100/xterm 의 부분집합, 셀 격자 + 스크롤백)
 *   taskbar.c 작업 표시줄
 *   wm.c     창 관리: 창 목록, 테두리, 포커스, 앱에 키/이벤트 넘기기
 *   nterminal.c  터미널 창 (pty + vt.c, 대화형 nysh 또는 명령 하나)
 *   nrunner.c    실행 대화상자 (명령을 창/최대화된 창에서 실행, 터미널/파일 탐색기 열기)
 *   nfiles.c     파일 탐색기
 *   menu.c       메뉴: 앱, 테마, 셸, 로그아웃, 전원 끄기
 *   clock.c      시계 / 달력 창 (작업 표시줄의 시계 위젯)
 *   main.c   진입점, 이벤트 루프, 전역 단축키
 */
#ifndef NTUI_H
#define NTUI_H

#include <stddef.h>
#include <sys/types.h>
#include <poll.h>

/* ---------------- sys.c: 세션 정보 ---------------- */

#define utcOffsetDefault 0               /* 분 */
#define timezoneNameDefault "Europe/London"
#define hostnameDefault "nyxis"

extern uid_t uid;
extern char username[64];
extern int utcOffset;                   /* UTC 오프셋 (분). POSIX <time.h> 의 timezone 과 겹치지 않게 */
extern char timezoneName[64];
extern char hostname[64];
extern char isRoot;

int init(void);

/* ---------------- term.c: 입력 ---------------- */

/*
 * 키 값 = 기본 키 | 수정자 비트.
 *   기본 키: 글자(Shift 반영, 0x20-0x7E), '\r' Enter, '\t' Tab, 0x7F Backspace, KEY_ESC, KEY_UP 등 특수 키.
 *   Ctrl+글자는 제어 문자(KEY_CTRL('t') = 0x14)가 기본 키다 (KEY_MOD_CTRL 은 붙지 않는다).
 *   글자에는 KEY_MOD_SHIFT 를 붙이지 않는다 (이미 대문자/기호로 반영). 특수 키와 Tab/Enter 에는 붙는다.
 *   Alt/Super 는 언제나 비트로 붙는다. 터미널 모드에서는 Super 가 없고, ESC + 글자는 Alt+글자로 읽는다.
 */
enum {
    KEY_ESC = 27,
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_DEL, KEY_PGUP, KEY_PGDN, KEY_INS,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_SUPER_TAP,                      /* Super 를 눌렀다 뗐다 (그 사이 다른 키 없음) */
    KEY_PRINT,                          /* PrintScreen (화면 모드) */
    KEY_MENUKEY,                        /* 메뉴(애플리케이션) 키 (화면 모드) */
    KEY_ALT_UP                          /* Alt 를 뗐다 (inputWantAltRelease 로 켰을 때만: 창 전환기) */
};

#define KEY_MOD_SHIFT 0x1000
#define KEY_MOD_CTRL  0x2000
#define KEY_MOD_ALT   0x4000
#define KEY_MOD_SUPER 0x8000
#define KEY_MODS      0xF000
#define KEY_BASE(k)   ((k) & 0x0FFF)

#define KEY_TIMEOUT (-2)                /* getchTimeout: 시간 안에 키가 없었다 */

#define KEY_CTRL(c)  ((c) & 0x1F)        /* KEY_CTRL('t') = 0x14 */

int inputOpen(void);                    /* 화면 모드면 이미 열린 화면의 키, 아니면 터미널 raw 모드 */
int inputFd(void);                      /* poll 로 기다릴 fd */
int getchTimeout(int ms);               /* 키 하나 (ms < 0 끝없이, 0 기다리지 않음). KEY_TIMEOUT / EOF */
int getch(void);
int inputPending(void);                 /* 화면 모드: 읽어 둔 키가 남았다 */
int inputDisplayMode(void);             /* 1 이면 화면 모드 (Super 단축키) */
void inputWantAltRelease(int on);       /* 화면 모드: Alt 를 떼면 KEY_ALT_UP 을 보낸다 */

/* 터미널 모드: ntui 가 쓰는 터미널 fd (dev:/tty). 화면 모드에서는 -1 */
int ttyFd(void);

/* ---------------- theme.c: 색 테마 ---------------- */

/* 그리는 코드가 쓰는 역할. 실제 색은 테마 표(theme.c)에만 있다 */
enum role {
    ROLE_DESKTOP, ROLE_WINDOW, ROLE_BORDER,
    ROLE_TITLE_ACTIVE, ROLE_TITLE_INACTIVE,
    ROLE_SELECTION, ROLE_TASKBAR, ROLE_ERROR,
    ROLE_COUNT
};

#define ATTR_BOLD      0x01U            /* 0-7 글자색은 밝은 색(8-15)으로 */
#define ATTR_REVERSE   0x02U
#define ATTR_UNDERLINE 0x04U

struct style {
    unsigned char fg, bg;               /* 256색 번호 */
    unsigned char fg16, bg16;           /* 16색 대체 (0-15) */
    unsigned char attr;                 /* ATTR_* */
};

struct theme {
    const char *name;                   /* "nyx", "daylight", "mono" (저장 파일에 쓰는 이름) */
    int only16;                         /* 1 이면 256색 화면에서도 16색 값만 쓴다 */
    struct style role[ROLE_COUNT];
};

extern const struct theme *currentTheme;

const struct theme *themeFind(const char *name);
void themeNext(void);
void themeLoad(void);
int themeSave(void);

/* ---------------- screen.c: 화면 ---------------- */

/*
 * 셀의 글자: 0x20-0x7E 는 ASCII, 그 밖에는 유니코드 상자 그리기(U+2500-257F)/블록(U+2580-259F) 일부.
 * 화면 모드는 선을 직접 그리고, 터미널 모드는 + - | # 로 바꾼다. 모르는 글자는 '?'.
 */
#define CH_HLINE  0x2500
#define CH_VLINE  0x2502
#define CH_TL     0x250C
#define CH_TR     0x2510
#define CH_BL     0x2514
#define CH_BR     0x2518

extern int screenCols;
extern int screenRows;
extern int screenColors;                /* 256 (기본) 또는 16 */

int screenInit(void);
void screenEnd(void);
void screenPut(int x, int y, int ch, enum role r);
void screenPutColor(int x, int y, int ch, int fg, int bg, unsigned attr);   /* 256색 번호 */
int screenText(int x, int y, const char *s, enum role r);
void screenFill(int x, int y, int w, int h, int ch, enum role r);
void screenClear(enum role r);
void screenBox(int x, int y, int w, int h, enum role r);
void screenStyle(enum role r, int *fg, int *bg, unsigned *attr);           /* 역할의 지금 색 */
void screenInvalidate(void);
void screenFlush(void);
int screenDisplayMode(void);            /* 1 이면 프레임버퍼에 직접 그린다 */
int screenShot(char *path, size_t cap); /* 화면 모드: 화면을 PPM 으로 저장 (path 에 이름). 성공 0 */
struct nx64_display;
struct nx64_display *screenDisplay(void);   /* 화면 모드의 화면 (아니면 NULL) */

/* ---------------- vt.c: 터미널 에뮬레이터 ---------------- */

#define VT_ATTR_BOLD      ATTR_BOLD
#define VT_ATTR_REVERSE   ATTR_REVERSE
#define VT_ATTR_UNDERLINE ATTR_UNDERLINE
#define VT_ATTR_FGDEF     0x10U         /* 기본 글자색 (그릴 때 창 색으로) */
#define VT_ATTR_BGDEF     0x20U         /* 기본 배경색 */

struct vtcell {
    unsigned short ch;
    unsigned char fg, bg;               /* 256색 번호 (VT_ATTR_*DEF 면 무시) */
    unsigned char attr;
};

struct vt;

/* 응답(커서 위치 보고 등)을 쓸 곳. ctx 는 vtNew 에 준 값 */
typedef void (*vt_reply_fn)(void *ctx, const char *s, int n);

struct vt *vtNew(int cols, int rows, vt_reply_fn reply, void *ctx);
void vtFree(struct vt *v);
void vtResize(struct vt *v, int cols, int rows);
void vtWrite(struct vt *v, const char *buf, int n);    /* 프로그램 출력을 해석한다 */
int vtCols(const struct vt *v);
int vtRows(const struct vt *v);
/* 보이는 줄 y (0..rows-1) 의 셀. scroll 만큼 스크롤백으로 올려 본다 (0 = 지금 화면) */
const struct vtcell *vtLine(const struct vt *v, int y, int scroll);
int vtScrollback(const struct vt *v);  /* 올려 볼 수 있는 줄 수 (보조 화면이면 0) */
int vtCursor(const struct vt *v, int *x, int *y);  /* 커서가 보이면 1 */
int vtAppCursorKeys(const struct vt *v);           /* DECCKM: 방향키를 ESC O A 로 */
/* 키를 프로그램에 보낼 바이트로 바꾼다. 바이트 수 (보낼 것이 없으면 0) */
int vtEncodeKey(const struct vt *v, int key, char *out, int cap);

/* ---------------- taskbar.c: 작업 표시줄 (맨 아래 한 줄) ---------------- */

#define TASKBAR_ROWS 1                  /* 창 영역은 screenRows - TASKBAR_ROWS 줄까지 */

void taskbarSetWindows(const char *const *names, const int *minimized, int count, int active);
int taskbarActive(void);
int taskbarFocused(void);
void taskbarToggleFocus(void);
void taskbarCycle(int step);
int taskbarTick(void);
void taskbarDraw(void);
int taskbarKey(int key);

/* ---------------- wm.c: 창 관리 ---------------- */

#define WM_MAX 12

struct window;

/* 앱 = 창 안쪽을 맡는 함수들 (NULL 이면 없음) */
struct app {
    const char *name;
    /* 안쪽 영역(테두리 안)에 그린다. active: 활성 창이면 1 */
    void (*draw)(struct window *w, int x, int y, int cols, int rows, int active);
    int (*key)(struct window *w, int key);                  /* 처리했으면 1 */
    int (*pollFds)(struct window *w, struct pollfd *fds, int max);  /* 기다릴 fd 를 채우고 개수를 돌려준다 */
    void (*ready)(struct window *w, const struct pollfd *p);        /* 그 fd 가 준비됐다 */
    int (*tick)(struct window *w);                          /* 루프가 깰 때마다. 바뀌었으면 1 */
    void (*close)(struct window *w);                        /* 창이 닫힌다: data 를 정리 */
};

struct window {
    char title[32];
    int x, y, w, h;                     /* 테두리를 포함한 위치와 크기 */
    int layout;                         /* LAYOUT_* */
    int prevLayout;                     /* 전체 화면 전의 배치 (F11 로 돌아갈 곳) */
    int minimized;                      /* 1 이면 그리지 않고 키도 주지 않는다 (앱은 계속 돈다) */
    int savedX, savedY, savedW, savedH;
    int wantClose;                      /* 1 이면 다음 wmReap 이 닫는다 */
    const struct app *app;
    void *data;                         /* 앱의 상태 */
};

#define LAYOUT_NORMAL 0
#define LAYOUT_MAX    1                 /* 창 영역 전체 */
#define LAYOUT_LEFT   2                 /* 왼쪽 절반 */
#define LAYOUT_RIGHT  3                 /* 오른쪽 절반 */
#define LAYOUT_FULL   4                 /* 화면 전체 (테두리, 작업 표시줄 없음) */

struct window *wmOpen(const char *title, const struct app *app, void *data, int w, int h);
int wmReap(void);
void wmCloseAll(void);
int wmCount(void);
struct window *wmActive(void);
void wmSetActive(int i);              /* 최소화된 창이면 되살린다 */
void wmMinimize(int i);
void wmMinimizeActive(void);
void wmCloseActive(void);
void wmToggleMaximize(struct window *w);
void wmSetLayout(struct window *w, int layout);
void wmSnap(int side);                /* -1 왼쪽 절반, +1 오른쪽 절반 */
void wmRestoreOrMinimize(void);
void wmToggleFullscreen(void);
int wmFullscreenActive(void);
void wmMinimizeAll(void);
void wmRestoreAll(void);
void wmShowDesktop(void);
void wmActivateNth(int n);
int wmFind(const char *title);
const char *wmTitle(int i);
int wmIsMinimized(int i);
int wmActiveIndex(void);
void wmStartMove(int resize);         /* 활성 창 옮기기(0)/크기 조절(1) 모드 */
void wmMenuOpen(void);                /* 창 메뉴 (Alt+Space) */
int wmMenuIsOpen(void);
void wmMenuDraw(void);
int wmMenuKey(int key);
int wmVisibleCount(void);
void wmSyncTaskbar(void);
void wmDraw(void);
int wmKey(int key);
int wmPollFds(struct pollfd *fds, int *owner, int max);
int wmPollDone(const struct pollfd *fds, const int *owner, int n);
int wmTick(void);

/* ---------------- nterminal.c / nrunner.c ---------------- */

#define RUN_MAXIMIZED 0x1               /* 최대화된 창으로 연다 */
#define RUN_AUTOCLOSE 0x2               /* 명령이 끝나면 창을 바로 닫는다 */

struct window *nterminalOpen(void);
struct window *nterminalOpenIn(const char *dir);
struct window *nterminalRun(const char *line);
/* 명령 하나를 창에서 실행한다. title 이 NULL 이면 명령의 첫 낱말. cols/rows 가 0 이면 기본 크기 (안쪽 글자 칸) */
struct window *nterminalRunEx(const char *line, int flags, const char *title, int cols, int rows);

void nrunnerOpen(void);
int nrunnerIsOpen(void);
void nrunnerDraw(void);
int nrunnerKey(int key);
/* 창 없이 명령을 실행하고 끝날 때까지 기다린다 (poweroff). 종료 코드, 실패 -1 */
int nrunnerExecHidden(const char *line);

/* ---------------- nfiles.c: 파일 탐색기 ---------------- */

struct window *nfilesOpen(const char *dir);

/* ---------------- menu.c: 메뉴 ---------------- */

#define MENU_IGNORED 0                  /* menuKey: 메뉴가 닫혀 있다 (키를 다른 곳에) */
#define MENU_DONE    1                  /* 메뉴가 키를 썼다 */
#define MENU_QUIT    2                  /* 로그아웃: ntui 를 끝낸다 */

void menuOpen(void);
void menuOpenAt(char key);
void menuClose(void);
int menuIsOpen(void);
void menuDraw(void);
int menuKey(int key);

/* main.c: 도움말 창, 단축키 이름 ("Super+T" / "^T") */
void helpOpen(void);
const char *shortcutLabel(int action);

/* 전역 동작 (main.c 의 단축키, 메뉴) */
enum action {
    ACT_NONE, ACT_MENU, ACT_RUN, ACT_TERMINAL, ACT_FILES, ACT_CLOSE, ACT_MAXIMIZE, ACT_MINIMIZE,
    ACT_MOVE, ACT_NEXT, ACT_PREV, ACT_TASKBAR, ACT_THEME, ACT_HELP,
    ACT_SWITCH_NEXT, ACT_SWITCH_PREV, ACT_TASKVIEW, ACT_SNAP_LEFT, ACT_SNAP_RIGHT, ACT_RESTORE_MIN,
    ACT_FULLSCREEN, ACT_DESKTOP, ACT_MINIMIZE_ALL, ACT_RESTORE_ALL, ACT_WINMENU, ACT_RESIZE, ACT_LOCK,
    ACT_TASKMGR, ACT_SCREENSHOT,
    ACT_COUNT
};

/* ---------------- switcher.c: 창 전환기 (Alt+Tab) ---------------- */

void switcherOpen(int step, int holdAlt);   /* holdAlt: Alt 를 떼면 고른 창으로 (아니면 Enter) */
int switcherIsOpen(void);
void switcherDraw(void);
int switcherKey(int key);

/* ---------------- lock.c: 화면 잠금 ---------------- */

void lockStart(void);
int lockIsActive(void);
void lockDraw(void);
int lockKey(int key);

/* ---------------- lineedit.c: 한 줄 입력 ---------------- */

struct lineedit {
    char *buf;
    int cap;                            /* buf 크기 (NUL 포함) */
    int len;
    int pos;
};

void leInit(struct lineedit *le, char *buf, int cap);
void leSet(struct lineedit *le, const char *s);
/* 편집 키면 처리하고 1: 글자, Backspace/Delete, <- -> Home End, Ctrl-A/E/U/K/W, Ctrl+Backspace, Ctrl+<- -> (낱말) */
int leKey(struct lineedit *le, int key);
/* [x, x+width) 에 그린다 (커서가 보이도록 민다). secret 이면 '*' 로 */
void leDraw(const struct lineedit *le, int x, int y, int width, int secret, int cursor);

/* ---------------- clock.c: 시계 / 달력 창 ---------------- */

struct window *clockOpen(void);

#endif /* NTUI_H */
