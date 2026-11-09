/*
 * main.c - ntui 진입점, 이벤트 루프, 전역 단축키
 *
 * 이벤트 루프: poll 로 키 입력(콘솔), 창들이 기다리는 fd(터미널의 명령 출력), 시간(시계)을 함께 기다린다.
 *
 * 단축키 (어느 창에서나):
 *   Ctrl-R  실행 (nrunner)          Ctrl-T  새 터미널 (nterminal)     Ctrl-W  창 닫기
 *   Ctrl-E  파일 탐색기 (nfiles)
 *   Ctrl-N  다음 창                 Ctrl-P  이전 창                   Ctrl-F  최대화 / 되돌리기
 *   Ctrl-O  창 옮기기 / 크기 조절 (방향키, Tab 으로 바꾸기, Enter 끝)
 *   Ctrl-B  작업 표시줄 (<- -> Enter, Esc)                            Ctrl-G  테마 바꾸기 (저장)
 *   Ctrl-Q  메뉴 (앱, 테마, 셸, 로그아웃, 전원 끄기: menu.c). ntui 를 끝내는 것은 메뉴의 Log out 뿐이다
 * 그 밖의 키는 활성 창이 받는다 (터미널 창에서는 Ctrl-C/Ctrl-D 도 그 창의 것).
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include "ntui.h"

#define POLL_MAX_FDS 16

/* ---------------- 도움말 창 ---------------- */

static const char *const g_help[] = {
    "Welcome to ntui.",
    "",
    "^R  run a command (Tab: window/full)",
    "^T  new terminal     ^E  files",
    "^W  close window     ^F  maximize",
    "^O  move window (Tab: resize)",
    "^N/^P  next/prev     ^B  taskbar",
    "^G  theme            ^Q  menu",
    "(log out / power off: ^Q menu)",
    NULL
};

static void helpDraw(struct window *w, int x, int y, int cols, int rows, int active)
{
    char line[64];
    int i;

    (void)w;
    (void)active;
    for (i = 0; g_help[i] != NULL && i + 1 < rows; i++) {
        strncpy(line, g_help[i], sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        if ((int)strlen(line) > cols - 2)
            line[cols > 2 ? cols - 2 : 0] = '\0';
        screenText(x + 1, y + 1 + i, line, ROLE_WINDOW);
    }
    if (i + 2 < rows) {
        snprintf(line, sizeof(line), "%s@%s  theme %s", username, hostname, currentTheme->name);
        if ((int)strlen(line) > cols - 2)
            line[cols > 2 ? cols - 2 : 0] = '\0';
        screenText(x + 1, y + 2 + i, line, ROLE_SELECTION);
    }
}

static const struct app g_helpApp = { "help", helpDraw, NULL, NULL, NULL, NULL, NULL };

void helpOpen(void)
{
    (void)wmOpen("help", &g_helpApp, NULL, 44, 14);
}

/* ---------------- 그리기 ---------------- */

static void draw(void)
{
    screenClear(ROLE_DESKTOP);
    if (wmCount() == 0)
        screenText(2, 1, "ntui  -  ^T terminal   ^E files   ^R run   ^Q menu", ROLE_DESKTOP);
    wmDraw();
    taskbarDraw();
    nrunnerDraw();
    menuDraw();
}

/* ---------------- 키 ---------------- */

/* 처리 순서: 실행 상자 / 메뉴(열려 있으면 전부) -> 전역 단축키 -> 작업 표시줄 -> 창 단축키/활성 창.
 * 끝내야 하면(메뉴의 Log out) 1 */
static int handleKey(int key)
{
    int m;

    if (nrunnerKey(key))
        return 0;
    m = menuKey(key);
    if (m == MENU_QUIT)
        return 1;
    if (m != MENU_IGNORED)
        return 0;

    switch (key) {
    case KEY_CTRL('q'):
        menuOpen();
        return 0;
    case KEY_CTRL('r'):
        nrunnerOpen();
        return 0;
    case KEY_CTRL('t'):
        (void)nterminalOpen();
        return 0;
    case KEY_CTRL('e'):
        (void)nfilesOpen(NULL);
        return 0;
    case KEY_CTRL('g'):
        themeNext();
        (void)themeSave();
        screenInvalidate();             /* 색만 바뀐 칸도 다시 보내야 한다 */
        return 0;
    }

    wmSyncTaskbar();
    if (taskbarKey(key)) {
        wmSetActive(taskbarActive());
        return 0;
    }
    (void)wmKey(key);
    return 0;
}

/* SIGPIPE: 끝난 명령의 stdin 에 쓰면 온다. 처리기(무시가 아닌)를 두면 띄우는 명령에게는 기본값으로 넘어간다 */
static void onSigpipe(int sig)
{
    (void)sig;
}

int main(void)
{
    struct sigaction sa;
    struct pollfd fds[POLL_MAX_FDS];
    int owner[POLL_MAX_FDS];
    int dirty = 1;

    if (init() < 0)
        return 1;
    themeLoad();
    if (screenInit() < 0) {
        fprintf(stderr, "ntui: screen init failed\n");
        return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onSigpipe;
    (void)sigaction(SIGPIPE, &sa, NULL);

    helpOpen();

    for (;;) {
        int n;
        int r;

        if (wmReap())
            dirty = 1;
        if (wmTick())
            dirty = 1;
        if (dirty) {
            wmSyncTaskbar();
            draw();
            screenFlush();
            dirty = 0;
        }

        fds[0].fd = ttyFd();
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        n = 1 + wmPollFds(fds + 1, owner + 1, POLL_MAX_FDS - 1);

        r = poll(fds, (nfds_t)n, 1000);
        if (r < 0)
            continue;                   /* 신호 (EINTR) */
        if (r == 0) {
            dirty = taskbarTick();      /* 분이 바뀌었을 때만 다시 그린다 */
            continue;
        }
        if (n > 1 && wmPollDone(fds + 1, owner + 1, n - 1))
            dirty = 1;
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            int key = getchTimeout(0);

            if (key == EOF)
                break;
            if (key != KEY_TIMEOUT) {
                dirty = 1;
                if (handleKey(key))
                    break;
            }
        }
    }

    wmCloseAll();
    screenEnd();
    return 0;
}
