/*
 * nterminal.c - ntui 의 간단한 터미널 창 (터미널 에뮬레이터가 아니다)
 *
 * 한 줄을 입력받아 `nysh -c <줄>` 로 실행하고, 출력(stdout + stderr, 파이프)을 줄 단위로 모아 창 너비에 맞춰
 * 접어서 보여 준다. 창 크기가 바뀌면(Ctrl-F 등) 그릴 때 다시 접으므로 따로 할 일이 없다.
 *   - 입력 줄도 창 너비에 맞춰 여러 줄로 접힌다. <- -> Home End Ctrl-A Del Backspace, Ctrl-U(지우기), ↑ ↓(기록)
 *   - 명령이 도는 동안 Enter 로 친 줄은 명령의 stdin 으로 보낸다. Ctrl-D 는 stdin 을 닫고(EOF),
 *     Ctrl-C 는 nysh 에 SIGINT 를 보낸다. 명령이 없을 때 Ctrl-D 는 창을 닫는다 (셸의 EOF 처럼).
 *   - PgUp / PgDn 으로 지난 출력 보기. 내장 명령: cd [경로], clear, exit
 *   - 출력의 이스케이프 시퀀스(색 등)와 제어 문자는 버린다. 탭은 8 칸.
 *   - 창마다 현재 디렉터리를 따로 가진다 (명령을 띄울 때만 잠깐 chdir 한다).
 *
 * 명령 창 (nterminalRun, nrunner 가 쓴다): 같은 창이지만 프롬프트가 없고, 연 순간 명령 하나를 실행한다.
 * 명령이 끝나면 "[done]" 을 보이고, 그 뒤 Enter 를 누르면 창이 닫힌다. 실행 중의 입력/Ctrl-C/Ctrl-D 는 같다.
 *
 * 한계: 커서 이동/화면 지우기를 쓰는 프로그램(kilo 등)은 여기서 쓸 수 없다 (nrunner 로 전체 화면에서 실행).
 * Ctrl-C 는 nysh 하나에만 간다: nysh 가 띄운 자식이 남으면 출력 파이프를 닫아 더 기다리지 않는다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

#include "ntui.h"

#define TERM_LINES    300               /* 기억하는 출력 줄 수 */
#define TERM_LINE_MAX 512               /* 논리 줄 하나의 최대 길이 (넘으면 다음 줄로) */
#define TERM_INPUT    256
#define TERM_HIST     16
#define TERM_PATH     256

extern char **environ;

struct term {
    char lines[TERM_LINES][TERM_LINE_MAX];
    int lens[TERM_LINES];
    int head;                           /* 가장 오래된 줄 */
    int count;                          /* 줄 수 (마지막 줄은 열려 있을 수 있다) */
    int open;                           /* 1 이면 마지막 줄이 아직 출력을 받는 중 */
    int esc;                            /* 출력 해석 상태: 0 보통, 1 ESC 뒤, 2 CSI 안 */

    char input[TERM_INPUT];
    int inLen;
    int inPos;
    char hist[TERM_HIST][TERM_INPUT];
    int histCount;
    int histBrowse;                     /* 0 = 지금 줄, k = k 번째 이전 줄 */

    char cwd[TERM_PATH];
    int scroll;                         /* 아래에서부터 건너뛸 화면 줄 수 (PgUp) */
    int cols;                           /* 마지막으로 그린 너비/높이 (PgUp 의 한 쪽 크기, COLUMNS) */
    int rows;

    int oneShot;                        /* 명령 창: 프롬프트 없이 명령 하나 */
    pid_t pid;                          /* 실행 중인 nysh (-1 = 없음) */
    int outFd;                          /* 명령의 stdout/stderr 를 읽는 끝 (-1 = 닫힘) */
    int inFd;                           /* 명령의 stdin 에 쓰는 끝 (-1 = 닫힘) */
};

/* ---------------- 출력 줄 ---------------- */

static char *lineAt(struct term *t, int i)
{
    return t->lines[(t->head + i) % TERM_LINES];
}

static int *lenAt(struct term *t, int i)
{
    return &t->lens[(t->head + i) % TERM_LINES];
}

/* 빈 줄을 새로 연다 (가득 차면 가장 오래된 줄을 버린다) */
static void newLine(struct term *t)
{
    if (t->count == TERM_LINES) {
        t->head = (t->head + 1) % TERM_LINES;
        t->count--;
    }
    *lenAt(t, t->count) = 0;
    t->count++;
    t->open = 1;
}

static void putChar(struct term *t, char c)
{
    int *len;

    if (!t->open || t->count == 0)
        newLine(t);
    len = lenAt(t, t->count - 1);
    if (*len >= TERM_LINE_MAX) {
        newLine(t);
        len = lenAt(t, t->count - 1);
    }
    lineAt(t, t->count - 1)[(*len)++] = c;
}

static void endLine(struct term *t)
{
    if (!t->open || t->count == 0)
        newLine(t);
    t->open = 0;
}

/* 한 줄을 통째로 출력에 (명령 줄 메아리, 안내) */
static void putLine(struct term *t, const char *s)
{
    if (t->open && t->count > 0 && *lenAt(t, t->count - 1) > 0)
        endLine(t);                     /* 개행 없이 끝난 출력 뒤에 붙지 않게 */
    t->open = 0;
    while (*s)
        putChar(t, *s++);
    endLine(t);
}

/* 명령 출력 바이트를 해석해 넣는다 */
static void feed(struct term *t, const char *buf, long n)
{
    long i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];

        if (t->esc == 1) {              /* ESC 다음 */
            t->esc = c == '[' ? 2 : 0;
            continue;
        }
        if (t->esc == 2) {              /* CSI: 끝 바이트(0x40-0x7E)까지 버린다 */
            if (c >= 0x40 && c <= 0x7E)
                t->esc = 0;
            continue;
        }
        if (c == 0x1B) {
            t->esc = 1;
        } else if (c == '\n') {
            endLine(t);
        } else if (c == '\t') {
            do
                putChar(t, ' ');
            while (*lenAt(t, t->count - 1) % 8 != 0);
        } else if (c == '\b') {
            if (t->open && t->count > 0 && *lenAt(t, t->count - 1) > 0)
                (*lenAt(t, t->count - 1))--;
        } else if (c >= 0x20 && c != 0x7F) {
            putChar(t, c < 0x80 ? (char)c : '?');
        }
        /* '\r' 와 그 밖의 제어 문자는 버린다 */
    }
}

/* ---------------- 명령 실행 ---------------- */

static void closeFd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/* nysh 가 끝났는지 확인하고, 끝났으면 정리한다. 끝났으면 1 */
static int reap(struct term *t, int block)
{
    int status = 0;
    pid_t r;
    char msg[48];

    if (t->pid < 0)
        return 0;
    do
        r = waitpid(t->pid, &status, block ? 0 : WNOHANG);
    while (r < 0 && errno == EINTR);
    if (r == 0)
        return 0;
    if (r > 0 && WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        sprintf(msg, "[exit %d]", WEXITSTATUS(status));
        putLine(t, msg);
    } else if (r > 0 && WIFSIGNALED(status)) {
        sprintf(msg, "[signal %d]", WTERMSIG(status));
        putLine(t, msg);
    }
    t->pid = -1;
    closeFd(&t->inFd);
    if (t->oneShot)
        putLine(t, "[done - Enter or ^W to close]");
    return 1;
}

static void builtinCd(struct term *t, const char *arg)
{
    char here[TERM_PATH];
    char msg[TERM_PATH + 32];

    if (getcwd(here, sizeof(here)) == NULL)
        here[0] = '\0';
    if (chdir(t->cwd) != 0 || chdir(*arg ? arg : "face:/") != 0) {
        sprintf(msg, "cd: %.200s: %s", *arg ? arg : "face:/", strerror(errno));
        putLine(t, msg);
    } else if (getcwd(t->cwd, sizeof(t->cwd)) == NULL) {
        strcpy(t->cwd, "face:/");
    }
    if (here[0])
        (void)chdir(here);
}

static void spawnCommand(struct term *t, const char *line)
{
    posix_spawn_file_actions_t fa;
    char *argv[4];
    char here[TERM_PATH];
    char num[16];
    int out[2];
    int in[2];
    int err;

    if (pipe(out) != 0)
        goto fail;
    if (pipe(in) != 0) {
        close(out[0]);
        close(out[1]);
        goto fail;
    }
    /* 우리 쪽 끝은 다른 명령에게 넘기지 않는다 (넘기면 그 명령이 끝날 때까지 EOF 가 오지 않는다) */
    (void)fcntl(out[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(out[1], F_SETFD, FD_CLOEXEC);
    (void)fcntl(in[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(in[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, out[1], 2);

    argv[0] = (char *)"nysh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)line;
    argv[3] = NULL;

    /* 창 크기와 현재 디렉터리를 물려준다 (PWD 는 posix_spawn 이 지금 디렉터리로 채운다) */
    sprintf(num, "%d", t->cols > 0 ? t->cols : 80);
    setenv("COLUMNS", num, 1);
    sprintf(num, "%d", t->rows > 0 ? t->rows : 24);
    setenv("LINES", num, 1);
    if (getcwd(here, sizeof(here)) == NULL)
        here[0] = '\0';
    (void)chdir(t->cwd);
    err = posix_spawnp(&t->pid, "nysh", &fa, NULL, argv, environ);
    if (here[0])
        (void)chdir(here);
    posix_spawn_file_actions_destroy(&fa);

    close(in[0]);
    close(out[1]);
    if (err != 0) {
        close(in[1]);
        close(out[0]);
        t->pid = -1;
        errno = err;
        goto fail;
    }
    t->outFd = out[0];
    t->inFd = in[1];
    return;

fail:
    {
        char msg[96];

        sprintf(msg, "nterminal: cannot run nysh: %s", strerror(errno));
        putLine(t, msg);
    }
}

/* ---------------- 입력 줄 ---------------- */

static int promptOf(struct term *t, char *buf, size_t cap)
{
    if (t->pid >= 0 || t->oneShot) {
        buf[0] = '\0';                  /* 명령이 도는 중(또는 명령 창): 입력은 명령의 stdin 으로 */
        return 0;
    }
    snprintf(buf, cap, "%s$ ", t->cwd);
    return (int)strlen(buf);
}

static void histAdd(struct term *t, const char *s)
{
    if (*s == '\0')
        return;
    if (t->histCount > 0 && strcmp(t->hist[0], s) == 0)
        return;
    memmove(t->hist[1], t->hist[0], (size_t)(TERM_HIST - 1) * TERM_INPUT);
    strcpy(t->hist[0], s);
    if (t->histCount < TERM_HIST)
        t->histCount++;
}

static void setInput(struct term *t, const char *s)
{
    strncpy(t->input, s, TERM_INPUT - 1);
    t->input[TERM_INPUT - 1] = '\0';
    t->inLen = (int)strlen(t->input);
    t->inPos = t->inLen;
}

static void submit(struct term *t, struct window *w)
{
    char prompt[TERM_PATH + 4];
    char echo[TERM_PATH + TERM_INPUT + 4];
    char *line = t->input;

    t->input[t->inLen] = '\0';
    t->scroll = 0;

    if (t->pid >= 0) {                  /* 실행 중인 명령의 stdin 으로 */
        putLine(t, line);
        if (t->inFd >= 0) {
            (void)write(t->inFd, line, (size_t)t->inLen);
            (void)write(t->inFd, "\n", 1);
        }
        setInput(t, "");
        return;
    }
    if (t->oneShot) {                   /* 명령이 끝난 명령 창: Enter 로 닫는다 */
        w->wantClose = 1;
        return;
    }

    promptOf(t, prompt, sizeof(prompt));
    sprintf(echo, "%s%s", prompt, line);
    putLine(t, echo);
    histAdd(t, line);
    t->histBrowse = 0;

    while (*line == ' ')
        line++;
    if (*line == '\0') {
        /* 빈 줄 */
    } else if (strcmp(line, "clear") == 0) {
        t->count = 0;
        t->open = 0;
    } else if (strcmp(line, "exit") == 0) {
        w->wantClose = 1;
    } else if (strcmp(line, "cd") == 0 || strncmp(line, "cd ", 3) == 0) {
        line += 2;
        while (*line == ' ')
            line++;
        builtinCd(t, line);
    } else {
        spawnCommand(t, line);
    }
    setInput(t, "");
}

/* ---------------- 앱 함수 ---------------- */

/* 문자열 s(길이 n)를 cols 너비로 접었을 때 화면 줄 수 */
static int wrappedRows(int n, int cols)
{
    return n == 0 ? 1 : (n + cols - 1) / cols;
}

static void termDraw(struct window *w, int x, int y, int cols, int rows, int active)
{
    struct term *t = (struct term *)w->data;
    char prompt[TERM_PATH + 4];
    int plen;
    int total;
    int inRows;
    int avail;
    int row;
    int skip;
    int i;
    int k;

    if (cols <= 0 || rows <= 0)
        return;
    t->cols = cols;
    t->rows = rows;

    /* 입력 줄: 프롬프트 + 입력 (+ 끝의 커서 자리) */
    plen = promptOf(t, prompt, sizeof(prompt));
    total = plen + t->inLen;
    inRows = total / cols + 1;
    avail = rows - inRows;
    for (k = 0; k < inRows; k++) {
        int from = k * cols;
        int yy = y + rows - inRows + k;
        int c;

        if (yy < y)
            continue;                   /* 입력이 창보다 길면 앞부분은 안 보인다 */
        for (c = 0; c < cols && from + c < total; c++) {
            int idx = from + c;
            char ch = idx < plen ? prompt[idx] : t->input[idx - plen];

            screenPut(x + c, yy, ch, idx < plen ? ROLE_BORDER : ROLE_WINDOW);
        }
    }
    if (active) {
        int cur = plen + t->inPos;
        int cy = y + rows - inRows + cur / cols;

        if (cy >= y)
            screenPut(x + cur % cols, cy, t->inPos < t->inLen ? t->input[t->inPos] : ' ', ROLE_SELECTION);
    }

    /* 출력: 마지막 줄부터 위로. 열려 있는 빈 마지막 줄은 그리지 않는다 */
    row = avail - 1;
    skip = t->scroll;
    for (i = t->count - 1; i >= 0 && row >= 0; i--) {
        int len = *lenAt(t, i);
        int r;

        if (i == t->count - 1 && t->open && len == 0)
            continue;
        r = wrappedRows(len, cols);
        for (k = r - 1; k >= 0 && row >= 0; k--) {
            int c;

            if (skip > 0) {
                skip--;
                continue;
            }
            for (c = 0; c < cols && k * cols + c < len; c++)
                screenPut(x + c, y + row, lineAt(t, i)[k * cols + c], ROLE_WINDOW);
            row--;
        }
    }
    if (t->scroll > 0)
        screenText(x + cols - 10, y, "[scrolled]", ROLE_SELECTION);
}

static int termKey(struct window *w, int key)
{
    struct term *t = (struct term *)w->data;
    int page = t->rows > 2 ? t->rows - 1 : 1;

    switch (key) {
    case '\r':
    case '\n':
        submit(t, w);
        return 1;
    case 0x7F:
    case 0x08:
        if (t->inPos > 0) {
            memmove(t->input + t->inPos - 1, t->input + t->inPos, (size_t)(t->inLen - t->inPos));
            t->inPos--;
            t->inLen--;
        }
        return 1;
    case KEY_DEL:
        if (t->inPos < t->inLen) {
            memmove(t->input + t->inPos, t->input + t->inPos + 1, (size_t)(t->inLen - t->inPos - 1));
            t->inLen--;
        }
        return 1;
    case KEY_LEFT:
        if (t->inPos > 0)
            t->inPos--;
        return 1;
    case KEY_RIGHT:
        if (t->inPos < t->inLen)
            t->inPos++;
        return 1;
    case KEY_HOME:
    case KEY_CTRL('a'):
        t->inPos = 0;
        return 1;
    case KEY_END:                       /* Ctrl-E 는 전역 단축키(파일 탐색기) */
        t->inPos = t->inLen;
        return 1;
    case KEY_CTRL('u'):
        setInput(t, "");
        return 1;
    case KEY_UP:
        if (t->pid < 0 && t->histBrowse < t->histCount) {
            t->histBrowse++;
            setInput(t, t->hist[t->histBrowse - 1]);
        }
        return 1;
    case KEY_DOWN:
        if (t->pid < 0 && t->histBrowse > 0) {
            t->histBrowse--;
            setInput(t, t->histBrowse ? t->hist[t->histBrowse - 1] : "");
        }
        return 1;
    case KEY_PGUP:
        t->scroll += page;
        return 1;
    case KEY_PGDN:
        t->scroll = t->scroll > page ? t->scroll - page : 0;
        return 1;
    case KEY_CTRL('c'):
        if (t->pid >= 0) {
            (void)kill(t->pid, SIGINT);
            putLine(t, "^C");
        } else {
            setInput(t, "");
        }
        return 1;
    case KEY_CTRL('d'):
        if (t->pid >= 0)
            closeFd(&t->inFd);          /* 명령에게 EOF */
        else if (t->inLen == 0)
            w->wantClose = 1;
        return 1;
    }

    if (key >= 0x20 && key < 0x7F && t->inLen < TERM_INPUT - 1) {
        memmove(t->input + t->inPos + 1, t->input + t->inPos, (size_t)(t->inLen - t->inPos));
        t->input[t->inPos++] = (char)key;
        t->inLen++;
        t->scroll = 0;
        return 1;
    }
    return 1;                           /* 터미널 창은 다른 키를 먹는다 (전역 단축키는 그 전에 처리된다) */
}

static int termPollFds(struct window *w, struct pollfd *fds, int max)
{
    struct term *t = (struct term *)w->data;

    if (t->outFd < 0 || max < 1)
        return 0;
    fds[0].fd = t->outFd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    return 1;
}

static void termReady(struct window *w, const struct pollfd *p)
{
    struct term *t = (struct term *)w->data;
    char buf[1024];
    long n;

    if (t->outFd < 0)
        return;
    n = read(t->outFd, buf, sizeof(buf));
    if (n > 0) {
        feed(t, buf, n);
        return;
    }
    if (n < 0 && errno == EINTR)
        return;
    (void)p;
    closeFd(&t->outFd);                 /* EOF: 명령(과 그 자식)이 모두 출력을 닫았다 */
    if (t->open && t->count > 0 && *lenAt(t, t->count - 1) > 0)
        endLine(t);
    (void)reap(t, 0);
}

/* 출력은 닫혔는데 아직 끝나지 않은 nysh 를 정리한다 */
static int termTick(struct window *w)
{
    struct term *t = (struct term *)w->data;

    if (t->pid >= 0 && t->outFd < 0)
        return reap(t, 0);
    return 0;
}

static void termClose(struct window *w)
{
    struct term *t = (struct term *)w->data;

    if (t->pid >= 0) {
        (void)kill(t->pid, SIGINT);
        closeFd(&t->outFd);
        closeFd(&t->inFd);
        (void)reap(t, 1);
    }
    closeFd(&t->outFd);
    closeFd(&t->inFd);
    free(t);
}

static const struct app g_termApp = {
    "nterminal", termDraw, termKey, termPollFds, termReady, termTick, termClose
};

static struct term *newTerm(void)
{
    struct term *t = (struct term *)calloc(1, sizeof(*t));

    if (t == NULL)
        return NULL;
    t->pid = -1;
    t->outFd = -1;
    t->inFd = -1;
    if (getcwd(t->cwd, sizeof(t->cwd)) == NULL)
        strcpy(t->cwd, "face:/");
    return t;
}

/* 터미널 창을 하나 연다 (현재 디렉터리는 ntui 의 것). 실패하면 NULL */
struct window *nterminalOpen(void)
{
    return nterminalOpenIn(NULL);
}

/* 디렉터리 dir 에서 터미널 창을 연다 (NULL 이면 ntui 의 현재 디렉터리) */
struct window *nterminalOpenIn(const char *dir)
{
    struct term *t = newTerm();
    struct window *w;
    static int seq;
    char title[32];

    if (t == NULL)
        return NULL;
    if (dir != NULL && dir[0] != '\0') {
        strncpy(t->cwd, dir, sizeof(t->cwd) - 1);
        t->cwd[sizeof(t->cwd) - 1] = '\0';
    }
    putLine(t, "nterminal: commands run with nysh -c. ^C stop, ^D EOF/close, PgUp/PgDn scroll");

    sprintf(title, "nterminal %d", ++seq);
    w = wmOpen(title, &g_termApp, t, screenCols * 3 / 4, (screenRows - TASKBAR_ROWS) * 3 / 4);
    if (w == NULL)
        free(t);
    return w;
}

/*
 * 명령 하나를 창 안에서 실행한다 (nrunner). 창 제목은 명령의 첫 낱말. 출력은 터미널 창과 같이 접히고,
 * 입력은 명령의 stdin 으로 간다. 실패하면 NULL
 */
struct window *nterminalRun(const char *line)
{
    struct term *t = newTerm();
    struct window *w;
    char title[32];
    size_t n = 0;

    if (t == NULL)
        return NULL;
    t->oneShot = 1;
    while (*line == ' ')
        line++;
    while (line[n] != '\0' && line[n] != ' ' && n < sizeof(title) - 1) {
        title[n] = line[n];
        n++;
    }
    title[n] = '\0';

    w = wmOpen(title, &g_termApp, t, screenCols * 2 / 3, (screenRows - TASKBAR_ROWS) * 2 / 3);
    if (w == NULL) {
        free(t);
        return NULL;
    }
    t->cols = w->w - 2;                 /* 첫 그리기 전이라도 COLUMNS/LINES 가 맞게 */
    t->rows = w->h - 2;
    spawnCommand(t, line);
    if (t->pid < 0)
        putLine(t, "[done - Enter or ^W to close]");
    return w;
}
