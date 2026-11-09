/*
 * nrunner.c - 실행 대화상자 (Ctrl-R)
 *
 * 화면 가운데에 한 줄 입력 상자를 띄운다 (열려 있는 동안 모든 키를 받는다).
 *   Enter  실행. "nterminal"(또는 "term")이면 터미널 창을, "files" 면 파일 탐색기를 연다. 그 밖의 명령은 `nysh -c` 로 실행하는데,
 *          두 가지 방법이 있다 (Tab 으로 바꾼다. 마지막에 고른 것을 기억한다):
 *          - window (기본): 명령 창을 연다 (nterminal 과 같은 창: 출력을 창 너비에 맞춰 접고, 입력은 명령의
 *            stdin 으로, Ctrl-C 로 멈춤). ntui 는 계속 쓸 수 있다.
 *          - full screen: ntui 가 콘솔을 원래 모드로 돌려주고, 명령을 새 프로세스 그룹으로 만들어 콘솔의
 *            포그라운드로 둔다 (Ctrl-C 가 명령만 끝낸다). 끝나면 키를 하나 기다렸다가 ntui 로 돌아온다.
 *            kilo 처럼 커서를 옮기며 화면 전체를 쓰는 프로그램은 이쪽으로 띄운다.
 *   Tab    window / full screen 바꾸기
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
static int g_len;
static int g_pos;
static char g_last[RUN_INPUT];          /* 이전에 실행한 줄 (↑) */
static char g_msg[80];                  /* 실패 안내 */
static int g_fullscreen;                /* 0 = 창 안에서, 1 = 전체 화면 */

void nrunnerOpen(void)
{
    g_open = 1;
    g_len = 0;
    g_pos = 0;
    g_input[0] = '\0';
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
    int start;
    int c;

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
    screenText(x + w - 16, y, g_fullscreen ? "[full screen]" : "     [window]", ROLE_SELECTION);

    /* 입력: 커서가 보이도록 가로로 민다 */
    start = g_pos >= inner ? g_pos - inner + 1 : 0;
    screenText(x + 2, y + 2, ">", ROLE_BORDER);
    for (c = 0; c < inner && start + c < g_len; c++)
        screenPut(x + 4 + c, y + 2, g_input[start + c], ROLE_WINDOW);
    screenPut(x + 4 + (g_pos - start), y + 2, g_pos < g_len ? g_input[g_pos] : ' ', ROLE_SELECTION);

    if (g_msg[0])
        screenText(x + 2, y + 4, g_msg, ROLE_ERROR);
    else
        screenText(x + 2, y + 4, "Enter run  Tab window/full  Esc cancel", ROLE_BORDER);
}

/* 명령을 전체 화면에서 실행하고 끝나면 돌아온다 (파일 탐색기도 kilo 를 이것으로 띄운다) */
void nrunnerExec(const char *line)
{
    (void)nrunnerExecEx(line, 1, NULL);
}

/*
 * nrunnerExec 의 바탕. pause: 끝난 뒤 종료 코드를 보이고 키를 기다릴지. banner: 시작할 때 보일 줄
 * (NULL 이면 "running `...`"). 명령의 종료 코드를 돌려준다 (실행 실패/신호면 -1).
 */
int nrunnerExecEx(const char *line, int pause, const char *banner)
{
    char *argv[4];
    pid_t pid;
    pid_t fg;
    int status = 0;
    int err;
    int code = -1;
    int tty = ttyFd();

    screenSuspend();
    ttySuspend();
    if (banner != NULL)
        printf("%s\n", banner);
    else
        printf("ntui: running `%s`\n", line);
    fflush(stdout);

    argv[0] = (char *)"nysh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)line;
    argv[3] = NULL;
    err = posix_spawnp(&pid, "nysh", NULL, NULL, argv, environ);
    if (err != 0) {
        printf("ntui: cannot run nysh: %s\n", strerror(err));
    } else {
        /* 명령만의 프로세스 그룹을 콘솔의 포그라운드로: Ctrl-C 가 ntui 는 건드리지 않는다 */
        fg = tcgetpgrp(tty);
        (void)setpgid(pid, pid);
        (void)tcsetpgrp(tty, pid);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
        (void)tcsetpgrp(tty, fg > 0 ? fg : 0);
        if (WIFEXITED(status))
            code = WEXITSTATUS(status);
        if (pause && WIFEXITED(status))
            printf("\nntui: exit %d", WEXITSTATUS(status));
        else if (pause && WIFSIGNALED(status))
            printf("\nntui: signal %d", WTERMSIG(status));
    }
    if (pause || err != 0) {
        printf(" -- press any key to return to ntui\n");
        fflush(stdout);
    }

    ttyResume();
    if (pause || err != 0)
        (void)getch();
    screenResume();
    return code;
}

static void run(void)
{
    const char *line = g_input;

    g_input[g_len] = '\0';
    while (*line == ' ')
        line++;
    if (*line == '\0') {
        g_open = 0;
        return;
    }
    strcpy(g_last, g_input);
    if (strcmp(line, "files") == 0 || strcmp(line, "nfiles") == 0) {
        g_open = 0;
        if (nfilesOpen(NULL) == NULL) {
            g_open = 1;
            strcpy(g_msg, "cannot open a window (too many windows?)");
        }
        return;
    }
    if (strcmp(line, "nterminal") == 0 || strcmp(line, "term") == 0) {
        g_open = 0;
        if (nterminalOpen() == NULL) {
            g_open = 1;                 /* 상자를 그대로 두고 알린다 */
            strcpy(g_msg, "cannot open a terminal (too many windows?)");
        }
        return;
    }
    g_open = 0;
    if (g_fullscreen) {
        nrunnerExec(line);
    } else if (nterminalRun(line) == NULL) {
        g_open = 1;
        strcpy(g_msg, "cannot open a window (too many windows?)");
    }
}

/* 열려 있으면 모든 키를 먹는다 (1) */
int nrunnerKey(int key)
{
    if (!g_open)
        return 0;
    g_msg[0] = '\0';

    switch (key) {
    case KEY_ESC:
    case KEY_CTRL('c'):
        g_open = 0;
        return 1;
    case '\r':
    case '\n':
        run();
        return 1;
    case '\t':
        g_fullscreen = !g_fullscreen;
        return 1;
    case 0x7F:
    case 0x08:
        if (g_pos > 0) {
            memmove(g_input + g_pos - 1, g_input + g_pos, (size_t)(g_len - g_pos));
            g_pos--;
            g_len--;
        }
        return 1;
    case KEY_DEL:
        if (g_pos < g_len) {
            memmove(g_input + g_pos, g_input + g_pos + 1, (size_t)(g_len - g_pos - 1));
            g_len--;
        }
        return 1;
    case KEY_LEFT:
        if (g_pos > 0)
            g_pos--;
        return 1;
    case KEY_RIGHT:
        if (g_pos < g_len)
            g_pos++;
        return 1;
    case KEY_HOME:
        g_pos = 0;
        return 1;
    case KEY_END:
        g_pos = g_len;
        return 1;
    case KEY_CTRL('u'):
        g_len = g_pos = 0;
        return 1;
    case KEY_UP:
        strcpy(g_input, g_last);
        g_len = g_pos = (int)strlen(g_input);
        return 1;
    }
    if (key >= 0x20 && key < 0x7F && g_len < RUN_INPUT - 1) {
        memmove(g_input + g_pos + 1, g_input + g_pos, (size_t)(g_len - g_pos));
        g_input[g_pos++] = (char)key;
        g_len++;
    }
    return 1;
}
