/*
 * nrunner.c - 실행 대화상자 (Super+R)
 *
 * 화면 가운데에 한 줄 입력 상자를 띄운다 (열려 있는 동안 모든 키를 받는다).
 *   Enter  실행. "terminal"(또는 "nterminal", "term")이면 터미널 창을, "files" 면 파일 탐색기를 연다. 그 밖의 명령은
 *          `nysh -c` 로 명령 창(터미널 창: pty 위의 진짜 터미널이라 kilo 같은 전체 화면 프로그램도 된다)에서 실행한다.
 *          Tab 으로 보통 창 / 최대화된 창을 바꾼다 (마지막에 고른 것을 기억한다).
 *   Tab    보통 창 / 최대화된 창
 *   ESC    닫기.  Backspace / Ctrl-U 지우기, <- -> Home End 이동, ↑ 이전에 실행한 줄
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>

#include "ntui.h"

#define RUN_INPUT 200

extern char **environ;

static int g_open;
static char g_input[RUN_INPUT];
static struct lineedit g_le;
static char g_last[RUN_INPUT];          /* 이전에 실행한 줄 (↑) */
static char g_msg[80];                  /* 실패 안내 */
static int g_fullscreen;                /* 0 = 보통 창, 1 = 최대화된 창 */

void nrunnerOpen(void)
{
    g_open = 1;
    leInit(&g_le, g_input, RUN_INPUT);
    g_msg[0] = '\0';
}

int nrunnerIsOpen(void)
{
    return g_open;
}

void nrunnerDraw(void)
{
    int w;
    int h = 6;
    int x;
    int y;
    int inner;

    if (!g_open)
        return;
    w = screenCols - 4 < 60 ? screenCols - 4 : 60;
    if (w < 20)
        w = 20;
    x = (screenCols - w) / 2;
    y = (screenRows - TASKBAR_ROWS - h) / 3;
    inner = w - 6;                      /* "> " 와 테두리 */

    screenFill(x, y, w, h, ' ', ROLE_WINDOW);
    screenBox(x, y, w, h, ROLE_BORDER);
    screenFill(x + 1, y, w - 2, 1, ' ', ROLE_TITLE_ACTIVE);
    screenText(x + 2, y, "Run", ROLE_TITLE_ACTIVE);
    screenText(x + w - 14, y, g_fullscreen ? "[maximized]" : "   [window]", ROLE_SELECTION);

    /* 입력: 커서가 보이도록 가로로 민다 */
    screenText(x + 2, y + 2, ">", ROLE_BORDER);
    leDraw(&g_le, x + 4, y + 2, inner, 0, 1);

    if (g_msg[0])
        screenText(x + 2, y + 4, g_msg, ROLE_ERROR);
    else
        screenText(x + 2, y + 4, "Enter run  Tab window/maximized  Esc cancel", ROLE_BORDER);
}

/* 창 없이 명령을 실행하고 끝날 때까지 기다린다 (메뉴의 전원 끄기). 종료 코드, 실행 실패/신호면 -1 */
int nrunnerExecHidden(const char *line)
{
    char *argv[4];
    pid_t pid;
    int status = 0;

    argv[0] = (char *)"nysh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)line;
    argv[3] = NULL;
    if (posix_spawnp(&pid, "nysh", NULL, NULL, argv, environ) != 0)
        return -1;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void run(void)
{
    const char *line = g_input;

    while (*line == ' ')
        line++;
    if (*line == '\0') {
        g_open = 0;
        return;
    }
    strcpy(g_last, g_input);
    g_open = 0;
    if (strcmp(line, "files") == 0) {
        (void)launchApp("nfiles");
        return;
    }
    if (strcmp(line, "nterminal") == 0 || strcmp(line, "term") == 0 || strcmp(line, "terminal") == 0) {
        (void)launchTerminal(NULL);
        return;
    }
    (void)launchAuto(line, g_fullscreen ? LAUNCH_MAXIMIZED : 0);
}

/* 열려 있으면 모든 키를 먹는다 (1) */
int nrunnerKey(int key)
{
    if (!g_open)
        return 0;
    g_msg[0] = '\0';
    if (key == ('\t' | KEY_MOD_SHIFT))
        key = '\t';
    if ((key & (KEY_MOD_ALT | KEY_MOD_SUPER)) != 0)
        return 1;                       /* 다른 단축키는 상자가 열려 있는 동안 먹는다 */

    switch (KEY_BASE(key)) {
    case KEY_ESC:
        g_open = 0;
        return 1;
    case '\r':
    case '\n':
        run();
        return 1;
    case '\t':
        g_fullscreen = !g_fullscreen;
        return 1;
    case KEY_UP:
        leSet(&g_le, g_last);
        return 1;
    default:
        break;
    }
    if (key == KEY_CTRL('c')) {
        g_open = 0;
        return 1;
    }
    (void)leKey(&g_le, key);
    return 1;
}
