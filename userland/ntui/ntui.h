/*
 * ntui.h - ntui 공용 선언
 *
 *   term.c   터미널: raw 모드, 키 입력(getch)
 *   sys.c    세션 정보: 사용자, 시간대 (init)
 *   theme.c  색 테마 (역할 -> 색)
 *   screen.c 화면: 셀 버퍼, 바뀐 칸만 내보내기
 *   taskbar.c 작업 표시줄
 *   wm.c     창 관리: 창 목록, 테두리, 포커스, 앱에 키/이벤트 넘기기
 *   nterminal.c  터미널 창 (nysh -c 로 실행, 출력을 창 너비에 맞춰 접기)
 *   nrunner.c    실행 대화상자 (창 안에서 또는 전체 화면으로 명령 실행, 터미널/파일 탐색기 열기)
 *   nfiles.c     파일 탐색기
 *   menu.c       메뉴 (Ctrl-Q): 앱, 테마, 셸, 로그아웃, 전원 끄기
 *   main.c   진입점, 이벤트 루프, 전역 단축키
 */
#ifndef NTUI_H
#define NTUI_H

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

/* info:/ 같은 "파일 하나에 값 하나" 를 읽는다 (끝의 줄바꿈은 뗀다). 성공 0, 실패 -1 */
int sysReadValue(const char *path, char *buf, size_t cap);

/* ---------------- term.c: 터미널 ---------------- */

/* getch 가 돌려주는 특수 키 (일반 바이트는 0..255 그대로) */
enum {
    KEY_ESC = 27,
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_DEL, KEY_PGUP, KEY_PGDN
};

#define KEY_TIMEOUT (-2)                /* getchTimeout: 시간 안에 키가 없었다 */

/* 자주 쓰는 Ctrl 조합 (raw 모드에서 그대로 오는 바이트) */
#define KEY_CTRL(c)  ((c) & 0x1F)        /* KEY_CTRL('t') = 0x14 */

int ttyOpen(void);
int ttyFd(void);
void ttySuspend(void);
void ttyResume(void);
int getchTimeout(int ms);
int getch(void);

/* ---------------- theme.c: 색 테마 ---------------- */

/* 그리는 코드가 쓰는 역할. 실제 색은 테마 표(theme.c)에만 있다 */
enum role {
    ROLE_DESKTOP, ROLE_WINDOW, ROLE_BORDER,
    ROLE_TITLE_ACTIVE, ROLE_TITLE_INACTIVE,
    ROLE_SELECTION, ROLE_TASKBAR, ROLE_ERROR,
    ROLE_COUNT
};

#define ATTR_BOLD    0x01U              /* Nyxis 콘솔에서는 밝은 색으로 보인다 */
#define ATTR_REVERSE 0x02U

struct style {
    unsigned char fg, bg;               /* 256색 번호 */
    unsigned char fg16, bg16;           /* 16색 대체 (0-15) */
    unsigned char attr;                 /* ATTR_* */
};

struct theme {
    const char *name;                   /* "nyx", "daylight", "mono" (저장 파일에 쓰는 이름) */
    int only16;                         /* 1 이면 256색 터미널에서도 16색 값만 쓴다 */
    struct style role[ROLE_COUNT];
};

extern const struct theme *currentTheme;

const struct theme *themeFind(const char *name);
void themeNext(void);
void themeLoad(void);
int themeSave(void);
int themeSgr(enum role r, int colors, char *buf, size_t cap);

/* ---------------- screen.c: 화면 ---------------- */

extern int screenCols;
extern int screenRows;
extern int screenColors;                /* 256 (기본) 또는 16 */

int screenInit(void);
void screenEnd(void);
void screenPut(int x, int y, int ch, enum role r);
int screenText(int x, int y, const char *s, enum role r);
void screenFill(int x, int y, int w, int h, int ch, enum role r);
void screenClear(enum role r);
void screenBox(int x, int y, int w, int h, enum role r);
void screenInvalidate(void);
void screenFlush(void);
void screenSuspend(void);
void screenResume(void);

/* ---------------- taskbar.c: 작업 표시줄 (맨 아래 한 줄) ---------------- */

#define TASKBAR_ROWS 1                  /* 창 영역은 screenRows - TASKBAR_ROWS 줄까지 */

void taskbarSetWindows(const char *const *names, int count, int active);
int taskbarActive(void);
int taskbarFocused(void);
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
    int maximized;
    int savedX, savedY, savedW, savedH;
    int wantClose;                      /* 1 이면 다음 wmReap 이 닫는다 */
    const struct app *app;
    void *data;                         /* 앱의 상태 */
};

struct window *wmOpen(const char *title, const struct app *app, void *data, int w, int h);
int wmReap(void);
void wmCloseAll(void);
int wmCount(void);
struct window *wmActive(void);
void wmSetActive(int i);
void wmSyncTaskbar(void);
void wmDraw(void);
int wmKey(int key);
int wmPollFds(struct pollfd *fds, int *owner, int max);
int wmPollDone(const struct pollfd *fds, const int *owner, int n);
int wmTick(void);

/* ---------------- nterminal.c / nrunner.c ---------------- */

struct window *nterminalOpen(void);
struct window *nterminalOpenIn(const char *dir);
struct window *nterminalRun(const char *line);

void nrunnerOpen(void);
void nrunnerExec(const char *line);
int nrunnerExecEx(const char *line, int pause, const char *banner);
int nrunnerIsOpen(void);
void nrunnerDraw(void);
int nrunnerKey(int key);

/* ---------------- nfiles.c: 파일 탐색기 ---------------- */

struct window *nfilesOpen(const char *dir);

/* ---------------- menu.c: 메뉴 ---------------- */

#define MENU_IGNORED 0                  /* menuKey: 메뉴가 닫혀 있다 (키를 다른 곳에) */
#define MENU_DONE    1                  /* 메뉴가 키를 썼다 */
#define MENU_QUIT    2                  /* 로그아웃: ntui 를 끝낸다 */

void menuOpen(void);
void menuClose(void);
int menuIsOpen(void);
void menuDraw(void);
int menuKey(int key);

/* main.c: 도움말 창 */
void helpOpen(void);

#endif /* NTUI_H */
