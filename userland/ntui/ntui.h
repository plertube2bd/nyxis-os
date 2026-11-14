/*
 * ntui.h - ntui (컴포지터) 공용 선언
 *
 * ntui 는 Wayland 컴포지터 같은 자리다: 화면과 키보드를 혼자 가지고, 창 프로그램(nterm, nfiles, nclock, nkeys ...)은
 * 따로 도는 프로세스로서 NWP(<nwp.h>, docs/NWP.md)로 연결해 자기 창의 내용만 그린다. ntui 가 하는 것은
 * 창 배치/테두리/포커스, 작업 표시줄, 메뉴, 실행 상자, 창 전환기, 화면 잠금, 단축키(키를 어디로 보낼지), 스크린샷.
 *
 * ntui 는 두 가지로 돈다 (screen.c / term.c 가 고른다):
 *   - 화면 모드 (기본): 로그인 셸(콘솔 세션)로 시작하면 dev:/console 을 가져와 프레임버퍼에 직접 그리고, 키보드의
 *     키 이벤트를 받는다 (Shift/Ctrl/Alt/Super 구분). 커널 콘솔은 그동안 화면에 그리지 않는다.
 *   - 터미널 모드: 화면을 가져올 수 없으면(셸에서 띄움, 프레임버퍼 없음) 지금 터미널 위에서 ANSI 로 그린다.
 *
 *   term.c    입력: 키 (화면 모드의 키 이벤트 / 터미널 모드의 바이트 해석) -> KEY_* + 수정자
 *   sys.c     세션 정보: 사용자, 시간대 (init)
 *   theme.c   색 테마 (역할 -> 색. 클라이언트에게도 보낸다)
 *   screen.c  화면: 셀 버퍼, 바뀐 칸만 그리기 (프레임버퍼 / ANSI)
 *   server.c  NWP 서버: 클라이언트 연결, 창(서피스), 버퍼, 키/크기/포커스 이벤트
 *   launch.c  창 프로그램 띄우기 (NWP_DISPLAY 를 물려준다)
 *   taskbar.c 작업 표시줄
 *   wm.c      창 관리: 창 목록, 테두리, 포커스, 배치
 *   nrunner.c 실행 상자
 *   menu.c    메뉴: 앱, 테마, 로그아웃, 전원 끄기
 *   switcher.c lock.c lineedit.c  창 전환기, 화면 잠금, 한 줄 입력 그리기
 *   keymap.c  단축키 설정 (nkeys 와 함께 쓴다: keymap.h)
 *   main.c    진입점, 이벤트 루프, 키를 어디로 보낼지
 */
#ifndef NTUI_H
#define NTUI_H

#include <stddef.h>
#include <sys/types.h>
#include <poll.h>
#include <nwp.h>

#include "keymap.h"

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

/* 키 값과 KEY_* 이름: keymap.h (= nwp.h 의 NWP_KEY_*) */

#define KEY_TIMEOUT (-2)                /* getchTimeout: 시간 안에 키가 없었다 */


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
    void (*requestClose)(struct window *w);                 /* 사용자가 닫으려 한다 (NULL 이면 바로 닫는다) */
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
void wmPlace(struct window *w, int x, int y, int ww, int wh);   /* 음수 = 그대로 (보통 배치가 된다) */
int wmAreaRows(void);                 /* 창 영역의 줄 수 (작업 표시줄을 뺀 것) */
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
int wmIndexOf(const struct window *w);   /* 없으면 -1 */
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

/* ---------------- nrunner.c: 실행 상자 ---------------- */

void nrunnerOpen(void);
int nrunnerIsOpen(void);
void nrunnerDraw(void);
int nrunnerKey(int key);
/* 창 없이 명령을 실행하고 끝날 때까지 기다린다 (poweroff). 종료 코드, 실패 -1 */
int nrunnerExecHidden(const char *line);

/* ---------------- launch.c: 창 프로그램 띄우기 ---------------- */

#define LAUNCH_MAXIMIZED 0x1            /* nterm -m */
#define LAUNCH_AUTOCLOSE 0x2            /* nterm -x: 명령이 끝나면 창을 닫는다 */

int launch(const char *const *argv);    /* NWP_DISPLAY 를 물려준 채로 띄운다. 성공 0 */
/* 명령을 터미널 창(nterm)에서. title 이 NULL 이면 명령의 첫 낱말. cols/rows 0 = 기본 크기. 성공 0 */
int launchCommand(const char *line, int flags, const char *title, int cols, int rows);
int launchTerminal(const char *dir);    /* 대화형 터미널 (dir: 시작 디렉터리, NULL 이면 지금) */
/* 자기 창을 여는 프로그램(nfiles, nclock ...)이면 그대로, 아니면 터미널 창에서 (실행 상자, 단축키의 exec) */
int launchAuto(const char *line, int flags);
int launchApp(const char *name);        /* 인자 없이 (nfiles, nclock, nkeys ...) */
void launchReap(void);                  /* 끝난 자식을 거둔다 (이벤트 루프가 부른다) */

/* ---------------- server.c: NWP 서버 (창 프로그램과의 연결) ---------------- */

int serverInit(void);                   /* svc:/<사용자>/nwp 를 만들고 NWP_DISPLAY 를 정한다 */
int serverPollFds(struct pollfd *fds, int max);  /* 기다릴 fd (채널, 클라이언트 파이프) */
int serverPollDone(const struct pollfd *fds, int n);   /* 처리했다. 다시 그려야 하면 1 */
int serverFlush(void);                  /* 쌓인 이벤트를 보낼 수 있는 만큼 보낸다 */
void serverThemeChanged(void);          /* 모든 클라이언트에 새 테마 */
int serverInhibited(void);              /* 활성 창이 단축키 없이 모든 키를 원한다 (NWP_REQ_INHIBIT) */
int serverSignalActive(int sig);        /* 활성 창의 클라이언트에 NWP_EV_SIGNAL. 보냈으면 1 */
int serverActivateApp(const char *app_id);  /* 그 앱의 창이 있으면 활성으로 하고 1 */
void serverShutdown(void);              /* 모든 클라이언트에 CLOSE 를 보내고 연결을 닫는다 (로그아웃) */
int serverConsumeReload(void);          /* 클라이언트가 설정을 다시 읽어 달라고 했다 (한 번만 1) */
const char *serverActiveAppId(void);    /* 활성 창의 앱 이름 (단축키의 @앱), 없으면 "" */

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

/* main.c: 잠깐 보이는 알림 */
void ntuiToast(const char *msg);
void runAction(int action);             /* 컴포지터 동작 (ACT_*: 단축키, 메뉴) */

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

/* ---------------- lineedit.c: 한 줄 입력 (편집은 nwp.h 의 nwp_le_*, 그리기만 여기) ---------------- */

#define lineedit nwp_lineedit
#define leInit   nwp_le_init
#define leSet    nwp_le_set
#define leKey    nwp_le_key
/* [x, x+width) 에 그린다 (커서가 보이도록 민다). secret 이면 '*' 로 */
void leDraw(const struct lineedit *le, int x, int y, int width, int secret, int cursor);

#endif /* NTUI_H */
