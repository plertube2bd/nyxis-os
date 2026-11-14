/*
 * nterm - 터미널 창 (NWP 클라이언트): 의사 터미널(openpty) + 터미널 에뮬레이터(vt.c)
 *
 *   nterm [-d 디렉터리] [-t 제목] [-g 열x행] [-m] [-f] [-x] [-e 명령]
 *     -d  시작 디렉터리        -t  창 제목 (기본 "terminal")     -g  바라는 크기 (예: 90x28)
 *     -m  최대화해서 연다      -f  전체 화면으로 연다
 *     -e  대화형 nysh 대신 `nysh -c 명령` 하나. 끝나면 "[exit N]" 을 보이고 Enter 로 닫는다
 *     -x  (-e 와 함께) 명령이 끝나면 창을 바로 닫는다
 *
 * 컴포지터(ntui) 위에서 도는 창 프로그램이다 (NWP_DISPLAY). pty 의 종속 끝을 0/1/2 로 해서 nysh 를 띄운다: 프로그램
 * 입장에서는 진짜 터미널이다 (줄 편집/에코/Ctrl-C 는 커널의 pty 줄 편집기, raw 모드의 kilo/sysmon/ntop 도 된다).
 *   - 컴포지터가 크기를 정하면(configure) pty 크기(TIOCSWINSZ)를 맞춘다: 포그라운드 프로그램이 SIGWINCH 를 받는다.
 *   - 키는 vtEncodeKey 로 바꿔 pty 에 쓴다. Shift+PgUp / Shift+PgDn 은 지난 출력(스크롤백) 보기.
 *   - 단축키의 signal 문장(NWP_EV_SIGNAL)은 pty 의 포그라운드 작업에 보낸다.
 *   - 창을 닫으면(Alt+F4 = NWP_EV_CLOSE) pty 주 끝을 닫는다: 그 터미널의 프로그램은 SIGHUP 을 받는다.
 *   - 프로그램이 이 창을 다룰 수 있다 (nx64api 의 nx64_win_*): xterm 의 창 조작 CSI Ps ; Ps ; Ps t 를 컴포지터 요청으로.
 *       1 되살리기  2 최소화  3;x;y 옮기기(칸)  8;행;열 크기(안쪽, 0 = 그대로)  9;0/9;1 보통/최대화
 *       10;0/10;1/10;2 전체 화면 끄기/켜기/바꾸기
 *       질의: 11 -> CSI 1;상태 t (최소화면 CSI 2;3 t, 상태 = NWP_STATE_*)  13 -> CSI 3;x;y t  18 -> CSI 8;행;열 t
 *             19 -> CSI 9;행;열 t (창 영역)
 *     OSC 0/2 는 창 제목. CSI > 0 q (XTVERSION) 에는 "nterm(1)" 로 답한다 (ntui 의 터미널인지 알아보는 데 쓴다).
 * 이 프로그램이 띄운 셸과 그 자식은 NWP_DISPLAY 를 물려받는다: 터미널에서 nfiles 를 치면 새 창이 뜬다.
 * 앱 이름(단축키의 @앱)은 "nterminal".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <poll.h>
#include <pty.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <nx64.h>
#include <nwp.h>

#include "vt.h"

#define TERM_PATH 256

extern char **environ;

struct term {
    struct nwp_window *w;
    struct vt *vt;
    int master;                         /* 주 끝 (-1 = 닫힘: 프로그램이 모두 끝났다) */
    pid_t pid;                          /* nysh (-1 = 끝남) */
    int oneShot;                        /* -e */
    int autoClose;                      /* -x */
    int done;                           /* -e: 명령이 끝났고 Enter 를 기다린다 */
    int scroll;                         /* 스크롤백으로 올려 본 줄 수 */
    int cols, rows;                     /* pty 에 알린 크기 */
    int dirty;
    int changed;                        /* 창을 바꾸는 요청을 보냈다: 질의 전에 컴포지터와 맞춘다 */
};

static struct term g_t;

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

static void say(struct term *t, const char *msg)
{
    vtWrite(t->vt, msg, (int)strlen(msg));
    t->dirty = 1;
}

/* ---------------- 프로그램의 창 조작 (CSI ... t) ---------------- */

/* 질의의 답: 숫자 두 개 */
static void replyf(struct term *t, const char *fmt, int a, int b)
{
    char buf[48];

    snprintf(buf, sizeof(buf), fmt, a, b);
    reply(t, buf, (int)strlen(buf));
}

static void winop(void *ctx, const int *p, int n)
{
    struct term *t = (struct term *)ctx;
    int a = n > 1 ? p[1] : 0;
    int b = n > 2 ? p[2] : 0;
    int state;

    switch (p[0]) {
    case 1:
        nwp_set_state(t->w, NWP_STATE_NORMAL);      /* 최소화돼 있으면 되살리기만 한다 */
        break;
    case 2:
        nwp_set_state(t->w, NWP_STATE_MINIMIZED);
        break;
    case 3:
        nwp_move(t->w, a, b);
        break;
    case 8:
        nwp_resize(t->w, b, a);                     /* xterm 순서: 행 ; 열 */
        break;
    case 9:
        nwp_set_state(t->w, a == 0 ? NWP_STATE_NORMAL : NWP_STATE_MAXIMIZED);
        break;
    case 10:
        if (a == 2)
            a = nwp_state(t->w) != NWP_STATE_FULLSCREEN;
        nwp_set_state(t->w, a ? NWP_STATE_FULLSCREEN : NWP_STATE_NORMAL);
        break;
    case 11: case 13: case 18: case 19:
        if (t->changed) {
            (void)nwp_roundtrip(t->w, 500);         /* 앞의 부탁의 결과를 받아 둔다 */
            t->changed = 0;
        }
        state = nwp_state(t->w);
        if (p[0] == 11)
            replyf(t, "\033[%d;%dt", state == NWP_STATE_MINIMIZED ? 2 : 1, state);
        else if (p[0] == 13)
            replyf(t, "\033[3;%d;%dt", nwp_x(t->w), nwp_y(t->w));
        else if (p[0] == 18)
            replyf(t, "\033[8;%d;%dt", nwp_rows(t->w), nwp_cols(t->w));
        else
            replyf(t, "\033[9;%d;%dt", nwp_area_rows(t->w), nwp_area_cols(t->w));
        return;
    default:
        return;                         /* 모르는 것 (22/23 제목 스택 등) */
    }
    t->changed = 1;
}

static void setTitle(void *ctx, const char *title)
{
    struct term *t = (struct term *)ctx;

    nwp_set_title(t->w, title);
}

/* nysh 를 pty 위에서 띄운다 (line 이 NULL 이면 대화형). 실패하면 -1 과 화면에 이유 */
static int spawnShell(struct term *t, const char *line)
{
    posix_spawn_file_actions_t fa;
    struct winsize ws;
    char *argv[4];
    char msg[128];
    int slave;
    int err;

    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)t->cols;
    ws.ws_row = (unsigned short)t->rows;
    if (openpty(&t->master, &slave, NULL, NULL, &ws) < 0) {
        snprintf(msg, sizeof(msg), "nterm: cannot create a terminal: %s\r\n", strerror(errno));
        say(t, msg);
        t->master = -1;
        return -1;
    }
    (void)fcntl(t->master, F_SETFD, FD_CLOEXEC);   /* 셸에게 넘기지 않는다 (넘기면 끊김이 오지 않는다) */
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
    err = posix_spawnp(&t->pid, "nysh", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(slave);                       /* 이제 종속 끝은 nysh(와 그 자식)만 가진다 */
    if (err != 0) {
        snprintf(msg, sizeof(msg), "nterm: cannot run nysh: %s\r\n", strerror(err));
        say(t, msg);
        close(t->master);
        t->master = -1;
        t->pid = -1;
        return -1;
    }
    return 0;
}

/* nysh 가 끝났는지 본다. 끝났으면 1 (-e 면 "[exit N]" 을 보인다) */
static int reap(struct term *t)
{
    int status = 0;
    pid_t r;
    char msg[64];

    if (t->pid < 0)
        return 0;
    do
        r = waitpid(t->pid, &status, WNOHANG);
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
    say(t, "\033[0m");
    say(t, msg);
    t->done = 1;
    return 1;
}

/* 포그라운드 작업(pty 의 포그라운드 프로세스 그룹)에 신호 */
static void signalJob(struct term *t, int sig)
{
    struct nx64_proc procs[64];
    long pg;
    int n;
    int i;

    if (t->master < 0)
        return;
    pg = nx64_pty_foreground(t->master);
    if (pg <= 0)
        return;                         /* 셸이 프롬프트에서 기다리는 중 */
    n = nx64_processes(procs, 64);
    for (i = 0; i < n && i < 64; i++) {
        if ((long)procs[i].pgid == pg)
            (void)nx64_signal_pid(procs[i].pid, sig);
    }
}

/* ---------------- 그리기 / 키 ---------------- */

static void draw(struct term *t)
{
    int dfg;
    int dbg;
    unsigned dattr;
    int cx;
    int cy;
    int cursor;
    int r;
    int c;
    int cols = nwp_cols(t->w);
    int rows = nwp_rows(t->w);

    setSize(t, cols, rows);
    if (t->scroll > vtScrollback(t->vt))
        t->scroll = vtScrollback(t->vt);
    nwp_style(t->w, NWP_ROLE_WINDOW, &dfg, &dbg, &dattr);
    cursor = vtCursor(t->vt, &cx, &cy) && nwp_focused(t->w) && t->scroll == 0 && !t->done;
    for (r = 0; r < rows && r < vtRows(t->vt); r++) {
        const struct vtcell *line = vtLine(t->vt, r, t->scroll);

        for (c = 0; c < cols && c < vtCols(t->vt); c++) {
            const struct vtcell *cell = &line[c];
            int fg = (cell->attr & VT_ATTR_FGDEF) ? dfg : cell->fg;
            int bg = (cell->attr & VT_ATTR_BGDEF) ? dbg : cell->bg;
            unsigned attr = cell->attr & (VT_ATTR_BOLD | VT_ATTR_UNDERLINE | VT_ATTR_REVERSE);

            if (cursor && r == cy && c == cx)
                attr ^= VT_ATTR_REVERSE;
            nwp_put(t->w, c, r, cell->ch, fg, bg, attr);
        }
    }
    if (t->scroll > 0) {
        char note[32];

        snprintf(note, sizeof(note), "[-%d]", t->scroll);
        nwp_text(t->w, cols - (int)strlen(note), 0, note, NWP_ROLE_SELECTION);
    }
    (void)nwp_commit(t->w);
    t->dirty = 0;
}

/* 키 하나. 창을 닫아야 하면 1 */
static int key(struct term *t, int k)
{
    char buf[16];
    int n;
    int page = t->rows > 2 ? t->rows - 1 : 1;

    t->dirty = 1;
    if (k == (KEY_PGUP | KEY_MOD_SHIFT)) {
        t->scroll += page;
        if (t->scroll > vtScrollback(t->vt))
            t->scroll = vtScrollback(t->vt);
        return 0;
    }
    if (k == (KEY_PGDN | KEY_MOD_SHIFT)) {
        t->scroll = t->scroll > page ? t->scroll - page : 0;
        return 0;
    }
    if (t->done)
        return k == '\r' || k == KEY_ESC;
    t->scroll = 0;
    if (t->master < 0)
        return 0;
    n = vtEncodeKey(t->vt, k, buf, (int)sizeof(buf));
    if (n > 0)
        (void)write(t->master, buf, (size_t)n);
    return 0;
}

/* ---------------- main ---------------- */

static void usage(void)
{
    fputs("usage: nterm [-d dir] [-t title] [-g COLSxROWS] [-m] [-f] [-x] [-e command]\n", stderr);
}

int main(int argc, char **argv)
{
    struct term *t = &g_t;
    const char *dir = NULL;
    const char *title = "terminal";
    const char *cmd = NULL;
    unsigned state = NWP_STATE_NORMAL;
    int cols = 0;
    int rows = 0;
    int quit = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-m") == 0)
            state = NWP_STATE_MAXIMIZED;
        else if (strcmp(a, "-f") == 0)
            state = NWP_STATE_FULLSCREEN;
        else if (strcmp(a, "-x") == 0)
            t->autoClose = 1;
        else if (i + 1 < argc && strcmp(a, "-d") == 0)
            dir = argv[++i];
        else if (i + 1 < argc && strcmp(a, "-t") == 0)
            title = argv[++i];
        else if (i + 1 < argc && strcmp(a, "-g") == 0) {
            if (sscanf(argv[++i], "%dx%d", &cols, &rows) != 2 || cols < 10 || rows < 3) {
                usage();
                return 2;
            }
        } else if (i + 1 < argc && strcmp(a, "-e") == 0)
            cmd = argv[++i];
        else {
            usage();
            return 2;
        }
    }
    if (dir != NULL && chdir(dir) != 0) {
        fprintf(stderr, "nterm: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    t->w = nwp_open("nterminal", title, cols, rows, state);
    if (t->w == NULL) {
        fprintf(stderr, "nterm: cannot open a window: %s (run it inside ntui)\n", strerror(errno));
        return 1;
    }
    t->master = -1;
    t->pid = -1;
    t->cols = nwp_cols(t->w);
    t->rows = nwp_rows(t->w);
    t->vt = vtNew(t->cols, t->rows, reply, t);
    if (t->vt == NULL)
        return 1;
    vtSetWindowOps(t->vt, winop, setTitle);
    t->oneShot = cmd != NULL;
    if (spawnShell(t, cmd) < 0 && t->oneShot)
        t->done = 1;
    draw(t);

    while (!quit) {
        struct pollfd fds[2];
        int n = 0;
        int r;

        fds[n].fd = nwp_fd(t->w);
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
        if (t->master >= 0) {
            fds[n].fd = t->master;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            n++;
        }
        r = poll(fds, (nfds_t)n, t->pid >= 0 && t->master < 0 ? 100 : 1000);
        if (r < 0 && errno != EINTR)
            break;

        /* 프로그램 출력 */
        if (n > 1 && (fds[1].revents & (POLLIN | POLLHUP))) {
            char buf[4096];
            ssize_t got = read(t->master, buf, sizeof(buf));

            if (got > 0) {
                vtWrite(t->vt, buf, (int)got);
                t->dirty = 1;
            } else if (got == 0 || errno != EINTR) {
                close(t->master);       /* EOF: 이 터미널을 쓰는 프로그램이 모두 끝났다 */
                t->master = -1;
            }
        }
        if (t->pid >= 0 && reap(t) && (!t->oneShot || t->autoClose))
            break;                      /* 셸이 끝났다: 창을 닫는다 */
        if (t->pid < 0 && t->master < 0 && !t->done && (!t->oneShot || t->autoClose))
            break;

        /* 컴포지터의 이벤트 */
        for (;;) {
            struct nwp_event ev;
            int got = nwp_next_event(t->w, &ev, 0);

            if (got < 0) {
                quit = 1;               /* 컴포지터가 없어졌다 */
                break;
            }
            if (got == 0)
                break;
            switch (ev.type) {
            case NWP_EVENT_KEY:
                if (key(t, ev.key))
                    quit = 1;
                break;
            case NWP_EVENT_RESIZE:
            case NWP_EVENT_THEME:
            case NWP_EVENT_FOCUS:
                t->dirty = 1;
                break;
            case NWP_EVENT_CLOSE:
                quit = 1;
                break;
            case NWP_EVENT_SIGNAL:
                signalJob(t, ev.sig);
                break;
            default:
                break;
            }
        }
        if (t->dirty && !quit)
            draw(t);
    }

    /* 끝: 주 끝을 닫으면 이 터미널의 프로그램들은 SIGHUP 을 받는다 */
    if (t->master >= 0)
        close(t->master);
    if (t->pid >= 0) {
        struct timespec ts = { 0, 10000000L };

        (void)kill(t->pid, SIGHUP);
        for (i = 0; i < 20 && !reap(t); i++)
            (void)nanosleep(&ts, NULL);
        if (t->pid >= 0)
            (void)kill(t->pid, SIGKILL);
    }
    nwp_close(t->w);
    vtFree(t->vt);
    return 0;
}
