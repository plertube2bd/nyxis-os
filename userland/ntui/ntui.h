/*
 * ntui.h - ntui 공용 선언
 *
 *   term.c   터미널: raw 모드, 키 입력(getch)
 *   sys.c    세션 정보: 사용자, 시간대 (init)
 *   theme.c  색 테마 (역할 -> 색)
 *   screen.c 화면: 셀 버퍼, 바뀐 칸만 내보내기
 *   taskbar.c 작업 표시줄 (TODO)
 *   main.c   진입점
 */
#ifndef NTUI_H
#define NTUI_H

#include <sys/types.h>

/* ---------------- sys.c: 세션 정보 ---------------- */

#define utcOffsetDefault 0               /* 분 */
#define timezoneNameDefault "Europe/London"

extern uid_t uid;
extern char username[64];
extern int utcOffset;                   /* UTC 오프셋 (분). POSIX <time.h> 의 timezone 과 겹치지 않게 */
extern char timezoneName[64];
extern char isRoot;

int init(void);

/* ---------------- term.c: 터미널 ---------------- */

/* getch 가 돌려주는 특수 키 (일반 바이트는 0..255 그대로) */
enum {
    KEY_ESC = 27,
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_DEL, KEY_PGUP, KEY_PGDN
};

int ttyOpen(void);
int ttyFd(void);
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

#endif /* NTUI_H */
