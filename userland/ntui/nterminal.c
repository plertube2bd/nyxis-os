/*
 * nterminal.c - 터미널 창: 의사 터미널(openpty) + 터미널 에뮬레이터(vt.c)
 *
 * 창마다 pty 를 하나 만들고, 종속 끝을 0/1/2 로 해서 nysh 를 띄운다. 프로그램 입장에서는 진짜 터미널이다:
 * 줄 편집/에코/Ctrl-C(SIGINT)는 커널의 pty 줄 편집기가, raw 모드(kilo, sysmon 같은 전체 화면 프로그램)도 그대로 된다.
 * 주 끝에서 읽은 출력은 vt.c 가 해석하고, 창에는 그 격자를 그린다. 키는 vtEncodeKey 로 바꿔 주 끝에 쓴다.
 *   - 창 크기가 바뀌면(최대화, 크기 조절) pty 크기(TIOCSWINSZ)를 맞춘다: 포그라운드 프로그램이 SIGWINCH 를 받는다.
 *   - Shift+PgUp / Shift+PgDn: 지난 출력(스크롤백) 보기. 다른 키를 치면 맨 아래로 돌아온다.
 *   - 셸(nysh)이 끝나면 창을 닫는다. 창을 닫으면 주 끝을 닫는다: 그 터미널의 프로세스는 SIGHUP 을 받는다.
 *
 * 명령 창 (nterminalRunEx: 실행 상자, 파일 탐색기의 보기/편집, 메뉴의 셸/시스템 모니터): `nysh -c <명령>` 하나를
 * 실행한다. 끝나면 RUN_AUTOCLOSE 면 바로 닫고, 아니면 "[exit N]" 을 보이고 Enter 로 닫는다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <pty.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

#include "ntui.h"

#define TERM_PATH 256

extern char **environ;

struct term {
    struct vt *vt;
    int master;                         /* 주 끝 (-1 = 닫힘: 프로그램이 모두 끝났다) */
    pid_t pid;                          /* nysh (-1 = 끝남) */
    int oneShot;                        /* 명령 창 */
    int autoClose;
    int done;                           /* 명령 창: 명령이 끝났고 Enter 를 기다린다 */
    int scroll;                         /* 스크롤백으로 올려 본 줄 수 */
    int cols, rows;                     /* pty 에 알린 크기 */
    char cwd[TERM_PATH];
};

/* ---------------- pty ---------------- */

static void reply(void *ctx, const char *s, int n)
{
    struct term *t = (struct term *)ctx;

    if (t->master >= 0)
        (void)write(t->master, s, (size_t)n);   /* 커서 위치 보고 등: 키 입력처럼 프로그램에게 */
}

static void setSize(struct term *t, int cols, int rows)
{
    struct winsize ws;

    if (cols == t->cols && rows == t->rows)
        return;
    t->cols = cols;
    t->rows = rows;
    vtResize(t->vt, cols, rows);
    if (t->master >= 0) {
        memset(&ws, 0, sizeof(ws));
        ws.ws_col = (unsigned short)cols;
        ws.ws_row = (unsigned short)rows;
        (void)ioctl(t->master, TIOCSWINSZ, &ws);
    }
}

/* nysh 를 pty 위에서 띄운다 (line 이 NULL 이면 대화형). 실패하면 -1 과 화면에 이유 */
static int spawnShell(struct term *t, const char *line)
{
    posix_spawn_file_actions_t fa;
    struct winsize ws;
    char *argv[4];
    char here[TERM_PATH];
    char msg[128];
    int slave;
    int err;

    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)t->cols;
    ws.ws_row = (unsigned short)t->rows;
    if (openpty(&t->master, &slave, NULL, NULL, &ws) < 0) {
        snprintf(msg, sizeof(msg), "nterminal: cannot create a terminal: %s\r\n", strerror(errno));
        vtWrite(t->vt, msg, (int)strlen(msg));
        t->master = -1;
        return -1;
    }
    (void)fcntl(t->master, F_SETFD, FD_CLOEXEC);   /* 다른 명령에게 넘기지 않는다 (넘기면 끊김이 오지 않는다) */
    (void)fcntl(slave, F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, slave, 0);
    posix_spawn_file_actions_adddup2(&fa, slave, 1);
    posix_spawn_file_actions_adddup2(&fa, slave, 2);

    argv[0] = (char *)"nysh";
    if (line != NULL) {
        argv[1] = (char *)"-c";
        argv[2] = (char *)line;
        argv[3] = NULL;
    } else {
        argv[1] = NULL;
    }

    if (getcwd(here, sizeof(here)) == NULL)
        here[0] = '\0';
    (void)chdir(t->cwd);
    err = posix_spawnp(&t->pid, "nysh", &fa, NULL, argv, environ);
    if (here[0])
        (void)chdir(here);
    posix_spawn_file_actions_destroy(&fa);
    close(slave);                       /* 이제 종속 끝은 nysh(와 그 자식)만 가진다 */

    if (err != 0) {
        snprintf(msg, sizeof(msg), "nterminal: cannot run nysh: %s\r\n", strerror(err));
        vtWrite(t->vt, msg, (int)strlen(msg));
        close(t->master);
        t->master = -1;
        t->pid = -1;
        return -1;
    }
    return 0;
}

/* nysh 가 끝났는지 본다 (block 이면 기다린다). 끝났으면 1 */
static int reap(struct term *t, int block)
{
    int status = 0;
    pid_t r;
    char msg[64];

    if (t->pid < 0)
        return 0;
    do
        r = waitpid(t->pid, &status, block ? 0 : WNOHANG);
    while (r < 0 && errno == EINTR);
    if (r == 0)
        return 0;
    t->pid = -1;
    if (!t->oneShot || t->autoClose)
        return 1;
    if (r > 0 && WIFEXITED(status))
        snprintf(msg, sizeof(msg), "\r\n[exit %d - Enter to close]", WEXITSTATUS(status));
    else if (r > 0 && WIFSIGNALED(status))
        snprintf(msg, sizeof(msg), "\r\n[signal %d - Enter to close]", WTERMSIG(status));
    else
        snprintf(msg, sizeof(msg), "\r\n[done - Enter to close]");
    vtWrite(t->vt, "\033[0m", 4);
    vtWrite(t->vt, msg, (int)strlen(msg));
    t->done = 1;
    return 1;
}

/* ---------------- 앱 함수 ---------------- */

static void termDraw(struct window *w, int x, int y, int cols, int rows, int active)
{
    struct term *t = (struct term *)w->data;
    int dfg;
    int dbg;
    unsigned dattr;
    int cx;
    int cy;
    int cursor;
    int r;
    int c;

    if (cols <= 0 || rows <= 0)
        return;
    setSize(t, cols, rows);
    if (t->scroll > vtScrollback(t->vt))
        t->scroll = vtScrollback(t->vt);

    screenStyle(ROLE_WINDOW, &dfg, &dbg, &dattr);
    cursor = vtCursor(t->vt, &cx, &cy) && active && t->scroll == 0 && !t->done;
    for (r = 0; r < rows && r < vtRows(t->vt); r++) {
        const struct vtcell *line = vtLine(t->vt, r, t->scroll);

        for (c = 0; c < cols && c < vtCols(t->vt); c++) {
            const struct vtcell *cell = &line[c];
            int fg = (cell->attr & VT_ATTR_FGDEF) ? dfg : cell->fg;
            int bg = (cell->attr & VT_ATTR_BGDEF) ? dbg : cell->bg;
            unsigned attr = cell->attr & (VT_ATTR_BOLD | VT_ATTR_UNDERLINE | VT_ATTR_REVERSE);

            if (cursor && r == cy && c == cx)
                attr ^= VT_ATTR_REVERSE;
            screenPutColor(x + c, y + r, cell->ch, fg, bg, attr);
        }
    }
    if (t->scroll > 0) {
        char note[32];

        snprintf(note, sizeof(note), "[-%d]", t->scroll);
        screenText(x + cols - (int)strlen(note), y, note, ROLE_SELECTION);
    }
}

static int termKey(struct window *w, int key)
{
    struct term *t = (struct term *)w->data;
    char buf[16];
    int n;
    int page = t->rows > 2 ? t->rows - 1 : 1;

    if (key == (KEY_PGUP | KEY_MOD_SHIFT)) {
        t->scroll += page;
        if (t->scroll > vtScrollback(t->vt))
            t->scroll = vtScrollback(t->vt);
        return 1;
    }
    if (key == (KEY_PGDN | KEY_MOD_SHIFT)) {
        t->scroll = t->scroll > page ? t->scroll - page : 0;
        return 1;
    }
    if (t->done) {
        if (key == '\r' || key == KEY_ESC)
            w->wantClose = 1;
        return 1;
    }
    t->scroll = 0;
    if (t->master < 0)
        return 1;
    n = vtEncodeKey(t->vt, key, buf, (int)sizeof(buf));
    if (n > 0)
        (void)write(t->master, buf, (size_t)n);
    return 1;                           /* 터미널 창은 모든 키를 먹는다 (전역 단축키는 그 전에 처리된다) */
}

static int termPollFds(struct window *w, struct pollfd *fds, int max)
{
    struct term *t = (struct term *)w->data;

    if (t->master < 0 || max < 1)
        return 0;
    fds[0].fd = t->master;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    return 1;
}

static void termReady(struct window *w, const struct pollfd *p)
{
    struct term *t = (struct term *)w->data;
    char buf[4096];
    ssize_t n;

    (void)p;
    if (t->master < 0)
        return;
    n = read(t->master, buf, sizeof(buf));
    if (n > 0) {
        vtWrite(t->vt, buf, (int)n);
        return;
    }
    if (n < 0 && errno == EINTR)
        return;
    /* EOF: 이 터미널을 쓰는 프로그램이 모두 끝났다 */
    close(t->master);
    t->master = -1;
    if (reap(t, 0) && (!t->oneShot || t->autoClose))
        w->wantClose = 1;
}

/* 출력은 닫히지 않았지만 nysh 가 끝났으면(뒤에 남은 프로그램이 있다) 정리한다 */
static int termTick(struct window *w)
{
    struct term *t = (struct term *)w->data;

    if (t->pid >= 0 && reap(t, 0)) {
        if (!t->oneShot || t->autoClose)
            w->wantClose = 1;
        return 1;
    }
    return 0;
}

static void termClose(struct window *w)
{
    struct term *t = (struct term *)w->data;
    int i;

    if (t->master >= 0) {
        close(t->master);               /* 끊김: 이 터미널의 프로그램들은 SIGHUP 을 받는다 */
        t->master = -1;
    }
    if (t->pid >= 0) {
        (void)kill(t->pid, SIGHUP);
        for (i = 0; i < 20 && !reap(t, 0); i++) {
            struct timespec ts = { 0, 10000000L };   /* 10ms: 끊김을 받고 끝날 시간 */

            (void)nanosleep(&ts, NULL);
        }
        if (t->pid >= 0) {
            (void)kill(t->pid, SIGKILL);
            (void)reap(t, 1);
        }
    }
    vtFree(t->vt);
    free(t);
}

static const struct app g_termApp = {
    "nterminal", termDraw, termKey, termPollFds, termReady, termTick, termClose
};

/* 터미널을 만들지 못한 창 (다음 wmReap 이 닫는다) */
static const struct app g_deadApp = { "nterminal", NULL, NULL, NULL, NULL, NULL, NULL };

/* ---------------- 열기 ---------------- */

static struct term *newTerm(int cols, int rows, const char *dir)
{
    struct term *t = (struct term *)calloc(1, sizeof(*t));

    if (t == NULL)
        return NULL;
    t->master = -1;
    t->pid = -1;
    t->cols = cols;
    t->rows = rows;
    t->vt = vtNew(cols, rows, reply, t);
    if (t->vt == NULL) {
        free(t);
        return NULL;
    }
    if (dir != NULL && dir[0] != '\0') {
        strncpy(t->cwd, dir, sizeof(t->cwd) - 1);
    } else if (getcwd(t->cwd, sizeof(t->cwd)) == NULL) {
        strcpy(t->cwd, "face:/");
    }
    return t;
}

static struct window *openTerm(const char *title, const char *dir, const char *line, int flags, int cols, int rows)
{
    struct window *w;
    struct term *t;
    int winW = cols > 0 ? cols + 2 : screenCols * 3 / 4;
    int winH = rows > 0 ? rows + 2 : (screenRows - TASKBAR_ROWS) * 3 / 4;

    w = wmOpen(title, &g_termApp, NULL, winW, winH);
    if (w == NULL)
        return NULL;
    if (flags & RUN_MAXIMIZED)
        wmToggleMaximize(w);
    t = newTerm(w->w - 2, w->h - 2, dir);   /* 첫 그리기 전이라도 pty 크기가 맞게 */
    if (t == NULL) {
        w->app = &g_deadApp;
        w->wantClose = 1;
        return NULL;
    }
    w->data = t;
    t->oneShot = line != NULL;
    t->autoClose = (flags & RUN_AUTOCLOSE) ? 1 : 0;
    if (spawnShell(t, line) < 0 && t->oneShot)
        t->done = 1;                    /* 이유를 보인 채 Enter 를 기다린다 */
    return w;
}

/* 터미널 창을 하나 연다 (현재 디렉터리는 ntui 의 것). 실패하면 NULL */
struct window *nterminalOpen(void)
{
    return nterminalOpenIn(NULL);
}

/* 디렉터리 dir 에서 터미널 창을 연다 (NULL 이면 ntui 의 현재 디렉터리) */
struct window *nterminalOpenIn(const char *dir)
{
    static int seq;
    char title[32];

    snprintf(title, sizeof(title), "terminal %d", ++seq);
    return openTerm(title, dir, NULL, 0, 0, 0);
}

struct window *nterminalRun(const char *line)
{
    return nterminalRunEx(line, 0, NULL, 0, 0);
}

struct window *nterminalRunEx(const char *line, int flags, const char *title, int cols, int rows)
{
    char name[32];
    size_t n = 0;

    while (*line == ' ')
        line++;
    if (title == NULL) {
        while (line[n] != '\0' && line[n] != ' ' && n < sizeof(name) - 1) {
            name[n] = line[n];
            n++;
        }
        name[n] = '\0';
        title = name;
    }
    return openTerm(title, NULL, line, flags, cols, rows);
}
