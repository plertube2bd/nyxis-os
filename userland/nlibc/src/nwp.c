/*
 * nwp.c - Nyxis Window Protocol 클라이언트 (규약은 library/nwp.h, docs/NWP.md)
 *
 * 창 하나 = 연결 하나. 클라이언트는 자기 칸(cells, malloc)에 그리고, nwp_commit 이 공유 메모리 버퍼로 복사해 COMMIT 을
 * 보낸다. 컴포지터가 그 버퍼를 복사해 가면 FRAME 이 온다: 그 전에 다시 commit 하면 기다렸다가 FRAME 때 보낸다
 * (컴포지터가 읽는 도중에 버퍼를 덮어쓰지 않게). 버퍼는 화면 크기(WELCOME 의 screen_cols x screen_rows)로 한 번만 만든다.
 *
 * 연결에 쓰는 파이프와 버퍼는 close-on-exec 이다: 이 프로그램이 띄운 자식(터미널의 셸)이 물려받으면 이 프로그램이
 * 끝나도 컴포지터가 끊김을 보지 못한다.
 */
#include <nwp.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "nl_internal.h"

#define NWP_IN_MAX 4096
#define NWP_QUEUE  32

struct nwp_window {
    int up;                             /* 요청을 쓴다 (위 파이프의 쓰기 끝) */
    int down;                           /* 이벤트를 읽는다 (아래 파이프의 읽기 끝) */
    long chan;                          /* 컴포지터 채널 (버퍼를 보낼 때) */
    char cookie[32];
    unsigned surface;
    int cols, rows;
    int state;
    int focused;
    unsigned flags;                     /* NWP_WELCOME_* */
    int screenCols, screenRows;
    int configured;
    int x, y;                           /* NWP_EV_PLACE (모르면 -1) */
    int areaCols, areaRows;
    unsigned syncSerial;                /* 마지막으로 보낸 NWP_REQ_SYNC */
    unsigned doneSerial;                /* 마지막으로 받은 NWP_EV_DONE */
    struct nwp_event queue[NWP_QUEUE];  /* nwp_roundtrip 이 읽어 둔 이벤트 */
    int qHead, qLen;

    struct nwp_cell *cells;             /* 그리는 곳 (cols x rows) */
    int shmFd;
    struct nwp_cell *shm;               /* 컴포지터와 나누는 버퍼 */
    size_t shmCells;
    unsigned bufferId;
    int attachedCols, attachedRows;
    int framePending;
    int commitWanted;

    unsigned char theme[NWP_ROLE_COUNT][3];
    unsigned char in[NWP_IN_MAX];
    size_t inLen;
};

/* 테마를 받기 전의 색 (nyx 테마) */
static const unsigned char g_defaultTheme[NWP_ROLE_COUNT][3] = {
    { 60, 232, 0 }, { 250, 234, 0 }, { 60, 234, 0 }, { 231, 54, NWP_ATTR_BOLD },
    { 244, 235, 0 }, { 231, 23, 0 }, { 248, 233, 0 }, { 231, 88, NWP_ATTR_BOLD },
};

int nwp_available(void)
{
    const char *d = getenv("NWP_DISPLAY");

    return d != NULL && d[0] != '\0';
}

/* ---------------- 보내기 ---------------- */

static int writeAll(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;

    while (n > 0) {
        ssize_t r = write(fd, p, n);

        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int sendReq(struct nwp_window *w, unsigned type, unsigned surface, const void *data, size_t len)
{
    unsigned char buf[sizeof(struct nwp_hdr) + NWP_MSG_MAX];
    struct nwp_hdr h;

    if (len > NWP_MSG_MAX) {
        errno = EINVAL;
        return -1;
    }
    h.type = (unsigned short)type;
    h.len = (unsigned short)len;
    h.surface = surface;
    memcpy(buf, &h, sizeof(h));
    if (len)
        memcpy(buf + sizeof(h), data, len);
    return writeAll(w->up, buf, sizeof(h) + len);    /* NWP_MSG_MAX 는 파이프의 원자적 쓰기보다 작다 */
}

/* 채널로 핸들을 붙여 보낸다 (큐가 차 있으면 잠깐씩 기다려 다시) */
static int sendHandle(struct nwp_window *w, const char *msg, int fd)
{
    long h = __nl_fd_to_handle(fd);
    int tries;

    if (h < 0)
        return -1;
    for (tries = 0; tries < 200; tries++) {
        if (__nl_channel_send_handle(w->chan, msg, strlen(msg), h) == 0)
            return 0;
        if (errno != EBUSY)
            return -1;
        (void)__nl_sleep_ms(10);
    }
    return -1;
}

/* ---------------- 버퍼 ---------------- */

static int makeBuffer(struct nwp_window *w, size_t cells)
{
    char msg[64];
    long h;

    h = __nl_shm_create(cells * sizeof(struct nwp_cell), 1);
    if (h < 0)
        return -1;
    w->shmFd = __nl_handle_to_fd(h);
    w->shm = (struct nwp_cell *)__nl_shm_map(h);
    if (w->shm == NULL) {
        (void)close(w->shmFd);
        w->shmFd = -1;
        return -1;
    }
    w->shmCells = cells;
    w->bufferId++;
    snprintf(msg, sizeof(msg), "nwp buf %s %u", w->cookie, w->bufferId);
    if (sendHandle(w, msg, w->shmFd) < 0)
        return -1;
    w->attachedCols = w->attachedRows = -1;
    return 0;
}

/* cols x rows 를 담을 버퍼가 있게 (모자라면 새로 만들고 옛것은 놓게 한다) */
static int ensureBuffer(struct nwp_window *w)
{
    size_t need = (size_t)w->cols * (size_t)w->rows;
    unsigned old = w->bufferId;
    int oldFd = w->shmFd;

    if (w->shm != NULL && need <= w->shmCells)
        return 0;
    if (need < (size_t)w->screenCols * (size_t)w->screenRows)
        need = (size_t)w->screenCols * (size_t)w->screenRows;
    w->shm = NULL;
    if (makeBuffer(w, need) < 0)
        return -1;
    if (old != 0) {
        (void)sendReq(w, NWP_REQ_BUFFER_FREE, 0, &old, sizeof(old));
        (void)close(oldFd);
    }
    return 0;
}

static int doCommit(struct nwp_window *w)
{
    if (ensureBuffer(w) < 0)
        return -1;
    if (w->attachedCols != w->cols || w->attachedRows != w->rows) {
        struct nwp_attach a;

        a.buffer = w->bufferId;
        a.cols = (unsigned short)w->cols;
        a.rows = (unsigned short)w->rows;
        a.format = NWP_FORMAT_CELLS;
        if (sendReq(w, NWP_REQ_ATTACH, w->surface, &a, sizeof(a)) < 0)
            return -1;
        w->attachedCols = w->cols;
        w->attachedRows = w->rows;
    }
    memcpy(w->shm, w->cells, sizeof(struct nwp_cell) * (size_t)w->cols * (size_t)w->rows);
    w->framePending = 1;
    w->commitWanted = 0;
    return sendReq(w, NWP_REQ_COMMIT, w->surface, NULL, 0);
}

int nwp_commit(struct nwp_window *w)
{
    if (w == NULL || !w->configured)
        return -1;
    if (w->framePending) {
        w->commitWanted = 1;            /* FRAME 이 오면 보낸다 */
        return 0;
    }
    return doCommit(w);
}

/* ---------------- 받기 ---------------- */

static int resizeCells(struct nwp_window *w, int cols, int rows)
{
    struct nwp_cell *c;
    size_t n;

    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;
    n = (size_t)cols * (size_t)rows;
    c = (struct nwp_cell *)calloc(n, sizeof(*c));
    if (c == NULL)
        return -1;
    free(w->cells);
    w->cells = c;
    w->cols = cols;
    w->rows = rows;
    return 0;
}

/* 버퍼의 메시지 하나를 처리한다. 1 = 앱에 줄 이벤트, 0 = 안에서 처리함, -2 = 메시지가 아직 다 오지 않았다 */
static int parseOne(struct nwp_window *w, struct nwp_event *ev)
{
    struct nwp_hdr h;
    unsigned char *p;
    size_t total;
    int out = 0;

    if (w->inLen < sizeof(h))
        return -2;
    memcpy(&h, w->in, sizeof(h));
    total = sizeof(h) + h.len;
    if (h.len > NWP_MSG_MAX)
        return -1;
    if (w->inLen < total)
        return -2;
    p = w->in + sizeof(h);
    memset(ev, 0, sizeof(*ev));

    switch (h.type) {
    case NWP_EV_WELCOME:
        if (h.len >= sizeof(struct nwp_welcome)) {
            struct nwp_welcome wl;

            memcpy(&wl, p, sizeof(wl));
            w->flags = wl.flags;
            w->screenCols = wl.screen_cols;
            w->screenRows = wl.screen_rows;
        }
        break;
    case NWP_EV_THEME:
        if (h.len >= sizeof(struct nwp_theme)) {
            struct nwp_theme t;
            int i;

            memcpy(&t, p, sizeof(t));
            for (i = 0; i < NWP_ROLE_COUNT; i++) {
                w->theme[i][0] = t.role[i][0];
                w->theme[i][1] = t.role[i][1];
                w->theme[i][2] = t.role[i][2];
            }
            if (w->configured) {
                ev->type = NWP_EVENT_THEME;
                out = 1;
            }
        }
        break;
    case NWP_EV_CONFIGURE:
        if (h.len >= sizeof(struct nwp_configure)) {
            struct nwp_configure c;

            memcpy(&c, p, sizeof(c));
            w->state = (int)c.state;
            if (!w->configured || c.cols != w->cols || c.rows != w->rows) {
                if (resizeCells(w, c.cols, c.rows) < 0)
                    return -1;
                ev->type = NWP_EVENT_RESIZE;
                ev->cols = w->cols;
                ev->rows = w->rows;
                out = w->configured;    /* 처음 것은 nwp_open 이 먹는다 */
            }
            w->configured = 1;
        }
        break;
    case NWP_EV_PLACE:
        if (h.len >= sizeof(struct nwp_place)) {
            struct nwp_place pl;

            memcpy(&pl, p, sizeof(pl));
            w->x = pl.x;
            w->y = pl.y;
            w->areaCols = pl.area_cols;
            w->areaRows = pl.area_rows;
        }
        break;
    case NWP_EV_DONE:
        if (h.len >= sizeof(unsigned))
            memcpy(&w->doneSerial, p, sizeof(unsigned));
        break;
    case NWP_EV_KEY:
        if (h.len >= sizeof(int)) {
            memcpy(&ev->key, p, sizeof(int));
            ev->type = NWP_EVENT_KEY;
            out = 1;
        }
        break;
    case NWP_EV_FOCUS:
        if (h.len >= sizeof(unsigned)) {
            unsigned on;

            memcpy(&on, p, sizeof(on));
            w->focused = on != 0;
            ev->type = NWP_EVENT_FOCUS;
            ev->focused = w->focused;
            out = 1;
        }
        break;
    case NWP_EV_CLOSE:
        ev->type = NWP_EVENT_CLOSE;
        out = 1;
        break;
    case NWP_EV_SIGNAL:
        if (h.len >= sizeof(unsigned)) {
            unsigned sig;

            memcpy(&sig, p, sizeof(sig));
            ev->type = NWP_EVENT_SIGNAL;
            ev->sig = (int)sig;
            out = 1;
        }
        break;
    case NWP_EV_FRAME:
        w->framePending = 0;
        if (w->commitWanted)
            (void)doCommit(w);
        break;
    case NWP_EV_ERROR:
        fprintf(stderr, "nwp: compositor: %.*s\n", (int)h.len, (const char *)p);
        break;
    default:
        break;                          /* 모르는 이벤트 (새 판의 컴포지터): 넘긴다 */
    }
    memmove(w->in, w->in + total, w->inLen - total);
    w->inLen -= total;
    return out;
}

int nwp_next_event(struct nwp_window *w, struct nwp_event *ev, int timeout_ms)
{
    int waited = 0;

    if (w == NULL || ev == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (w->qLen > 0) {                  /* nwp_roundtrip 이 읽어 둔 것부터 */
        *ev = w->queue[w->qHead];
        w->qHead = (w->qHead + 1) % NWP_QUEUE;
        w->qLen--;
        return 1;
    }
    for (;;) {
        struct pollfd p;
        ssize_t n;
        int r = parseOne(w, ev);

        if (r == 1)
            return 1;
        if (r == 0)
            continue;
        if (r == -1)
            return -1;
        /* 더 읽어야 한다. 한 번 읽은 뒤에는 기다리지 않는다 (앱의 poll 이 다시 깨운다) */
        p.fd = w->down;
        p.events = POLLIN;
        p.revents = 0;
        r = poll(&p, 1, waited ? (timeout_ms < 0 ? -1 : 0) : timeout_ms);
        if (r < 0 && errno == EINTR)
            return 0;
        if (r < 0)
            return -1;
        if (r == 0)
            return 0;
        n = read(w->down, w->in + w->inLen, sizeof(w->in) - w->inLen);
        if (n < 0 && errno == EINTR)
            return 0;
        if (n <= 0)
            return -1;                  /* 컴포지터가 없어졌다 */
        w->inLen += (size_t)n;
        waited = 1;
    }
}

int nwp_roundtrip(struct nwp_window *w, int timeout_ms)
{
    unsigned serial;
    long start = __nl_clock_ns(NL_CLOCK_MONOTONIC) / 1000000L;

    if (w == NULL) {
        errno = EINVAL;
        return -1;
    }
    serial = ++w->syncSerial;
    if (sendReq(w, NWP_REQ_SYNC, 0, &serial, sizeof(serial)) < 0)
        return -1;
    for (;;) {
        struct nwp_event ev;
        struct pollfd p;
        ssize_t n;
        long left;
        int r = parseOne(w, &ev);

        if (r == 1) {
            if (w->qLen < NWP_QUEUE) {
                w->queue[(w->qHead + w->qLen) % NWP_QUEUE] = ev;
                w->qLen++;
            }
            continue;
        }
        if (r == 0) {
            if (w->doneSerial == serial)
                return 0;
            continue;
        }
        if (r == -1)
            return -1;
        if (w->doneSerial == serial)
            return 0;
        left = timeout_ms - (__nl_clock_ns(NL_CLOCK_MONOTONIC) / 1000000L - start);
        if (left <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        p.fd = w->down;
        p.events = POLLIN;
        p.revents = 0;
        r = poll(&p, 1, (int)left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            if (r == 0)
                errno = ETIMEDOUT;
            return -1;
        }
        n = read(w->down, w->in + w->inLen, sizeof(w->in) - w->inLen);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        w->inLen += (size_t)n;
    }
}

/* ---------------- 열기 / 닫기 ---------------- */

struct nwp_window *nwp_open(const char *app_id, const char *title, int cols, int rows, unsigned state)
{
    static unsigned g_seq;
    const char *display = getenv("NWP_DISPLAY");
    struct nwp_window *w;
    struct nwp_hello hello;
    struct nwp_create cr;
    long up[2];
    long down[2];
    char msg[64];
    int i;

    if (display == NULL || display[0] == '\0') {
        errno = ENOENT;
        return NULL;
    }
    w = (struct nwp_window *)calloc(1, sizeof(*w));
    if (w == NULL)
        return NULL;
    w->up = w->down = w->shmFd = -1;
    w->x = w->y = -1;
    w->chan = -1;
    memcpy(w->theme, g_defaultTheme, sizeof(w->theme));
    snprintf(w->cookie, sizeof(w->cookie), "%ld.%u", __nl_getpid(), ++g_seq);

    w->chan = __nl_channel_open(display);
    if (w->chan < 0)
        goto fail;
    (void)__nl_handle_cloexec(w->chan, 1);
    if (__nl_pipe(up, 1) < 0)
        goto fail;
    if (__nl_pipe(down, 1) < 0) {
        (void)__nl_close(up[0]);
        (void)__nl_close(up[1]);
        goto fail;
    }
    w->up = __nl_handle_to_fd(up[1]);
    w->down = __nl_handle_to_fd(down[0]);
    snprintf(msg, sizeof(msg), "nwp up %s", w->cookie);
    i = sendHandle(w, msg, __nl_handle_to_fd(up[0]));
    if (i == 0) {
        snprintf(msg, sizeof(msg), "nwp down %s", w->cookie);
        i = sendHandle(w, msg, __nl_handle_to_fd(down[1]));
    }
    (void)close(__nl_handle_to_fd(up[0]));        /* 이제 컴포지터의 것 */
    (void)close(__nl_handle_to_fd(down[1]));
    if (i < 0)
        goto fail;

    memset(&hello, 0, sizeof(hello));
    hello.version = NWP_VERSION;
    hello.pid = (unsigned)__nl_getpid();
    snprintf(hello.app_id, sizeof(hello.app_id), "%s", app_id ? app_id : "");
    memset(&cr, 0, sizeof(cr));
    cr.cols = (unsigned short)(cols > 0 ? cols : 0);
    cr.rows = (unsigned short)(rows > 0 ? rows : 0);
    cr.state = state;
    snprintf(cr.title, sizeof(cr.title), "%s", title ? title : (app_id ? app_id : ""));
    w->surface = 1;
    if (sendReq(w, NWP_REQ_HELLO, 0, &hello, sizeof(hello)) < 0 ||
        sendReq(w, NWP_REQ_CREATE, w->surface, &cr, sizeof(cr)) < 0)
        goto fail;

    /* 첫 configure 까지 (WELCOME, THEME 이 먼저 온다) */
    for (i = 0; i < 30 && !w->configured; i++) {
        struct nwp_event ev;

        if (nwp_next_event(w, &ev, 100) < 0)
            goto fail;
    }
    if (!w->configured) {
        errno = ETIMEDOUT;
        goto fail;
    }
    if (ensureBuffer(w) < 0)
        goto fail;
    return w;

fail:
    {
        int e = errno ? errno : EIO;

        nwp_close(w);
        errno = e;
    }
    return NULL;
}

void nwp_close(struct nwp_window *w)
{
    if (w == NULL)
        return;
    if (w->up >= 0 && w->configured)
        (void)sendReq(w, NWP_REQ_DESTROY, w->surface, NULL, 0);
    if (w->up >= 0)
        (void)close(w->up);
    if (w->down >= 0)
        (void)close(w->down);
    if (w->shmFd >= 0)
        (void)close(w->shmFd);
    if (w->chan >= 0)
        (void)__nl_close(w->chan);
    free(w->cells);
    free(w);
}

int nwp_fd(const struct nwp_window *w)
{
    return w ? w->down : -1;
}

int nwp_cols(const struct nwp_window *w)
{
    return w ? w->cols : 0;
}

int nwp_rows(const struct nwp_window *w)
{
    return w ? w->rows : 0;
}

int nwp_focused(const struct nwp_window *w)
{
    return w ? w->focused : 0;
}

int nwp_display_mode(const struct nwp_window *w)
{
    return w ? (w->flags & NWP_WELCOME_DISPLAY) != 0 : 0;
}

void nwp_set_title(struct nwp_window *w, const char *title)
{
    char t[32];

    memset(t, 0, sizeof(t));
    snprintf(t, sizeof(t), "%s", title ? title : "");
    (void)sendReq(w, NWP_REQ_TITLE, w->surface, t, sizeof(t));
}

void nwp_set_state(struct nwp_window *w, unsigned state)
{
    (void)sendReq(w, NWP_REQ_STATE, w->surface, &state, sizeof(state));
}

void nwp_move(struct nwp_window *w, int x, int y)
{
    struct nwp_move m;

    if (w == NULL)
        return;
    m.x = (short)x;
    m.y = (short)y;
    (void)sendReq(w, NWP_REQ_MOVE, w->surface, &m, sizeof(m));
}

void nwp_resize(struct nwp_window *w, int cols, int rows)
{
    struct nwp_resize r;

    if (w == NULL || cols < 0 || rows < 0)
        return;
    r.cols = (unsigned short)cols;
    r.rows = (unsigned short)rows;
    (void)sendReq(w, NWP_REQ_RESIZE, w->surface, &r, sizeof(r));
}

int nwp_state(const struct nwp_window *w)
{
    return w ? w->state : NWP_STATE_NORMAL;
}

int nwp_x(const struct nwp_window *w)
{
    return w ? w->x : -1;
}

int nwp_y(const struct nwp_window *w)
{
    return w ? w->y : -1;
}

int nwp_area_cols(const struct nwp_window *w)
{
    return w ? w->areaCols : 0;
}

int nwp_area_rows(const struct nwp_window *w)
{
    return w ? w->areaRows : 0;
}

void nwp_inhibit_shortcuts(struct nwp_window *w, int on)
{
    unsigned v = on ? 1U : 0U;

    (void)sendReq(w, NWP_REQ_INHIBIT, w->surface, &v, sizeof(v));
}

void nwp_reload_config(struct nwp_window *w)
{
    (void)sendReq(w, NWP_REQ_RELOAD, 0, NULL, 0);
}

/* ---------------- 그리기 ---------------- */

void nwp_style(const struct nwp_window *w, int role, int *fg, int *bg, unsigned *attr)
{
    const unsigned char *s = (role >= 0 && role < NWP_ROLE_COUNT) ? w->theme[role] : w->theme[NWP_ROLE_WINDOW];

    *fg = s[0];
    *bg = s[1];
    *attr = s[2];
}

void nwp_put(struct nwp_window *w, int x, int y, int ch, int fg, int bg, unsigned attr)
{
    struct nwp_cell *c;

    if (w == NULL || w->cells == NULL || x < 0 || y < 0 || x >= w->cols || y >= w->rows)
        return;
    if (ch < 0x20 || ch == 0x7F || ch > 0xFFFF)
        ch = ch == 0 ? ' ' : '?';
    c = &w->cells[y * w->cols + x];
    c->ch = (unsigned short)ch;
    c->fg = (unsigned char)fg;
    c->bg = (unsigned char)bg;
    c->attr = (unsigned char)attr;
}

void nwp_put_role(struct nwp_window *w, int x, int y, int ch, int role)
{
    int fg;
    int bg;
    unsigned attr;

    nwp_style(w, role, &fg, &bg, &attr);
    nwp_put(w, x, y, ch, fg, bg, attr);
}

int nwp_text(struct nwp_window *w, int x, int y, const char *s, int role)
{
    while (*s) {
        nwp_put_role(w, x, y, (unsigned char)*s, role);
        s++;
        x++;
    }
    return x;
}

void nwp_fill(struct nwp_window *w, int x, int y, int cw, int ch, int c, int role)
{
    int i;
    int j;

    for (j = 0; j < ch; j++) {
        for (i = 0; i < cw; i++)
            nwp_put_role(w, x + i, y + j, c, role);
    }
}

void nwp_box(struct nwp_window *w, int x, int y, int bw, int bh, int role)
{
    int i;

    if (bw < 2 || bh < 2)
        return;
    for (i = 1; i < bw - 1; i++) {
        nwp_put_role(w, x + i, y, NWP_CH_HLINE, role);
        nwp_put_role(w, x + i, y + bh - 1, NWP_CH_HLINE, role);
    }
    for (i = 1; i < bh - 1; i++) {
        nwp_put_role(w, x, y + i, NWP_CH_VLINE, role);
        nwp_put_role(w, x + bw - 1, y + i, NWP_CH_VLINE, role);
    }
    nwp_put_role(w, x, y, NWP_CH_TL, role);
    nwp_put_role(w, x + bw - 1, y, NWP_CH_TR, role);
    nwp_put_role(w, x, y + bh - 1, NWP_CH_BL, role);
    nwp_put_role(w, x + bw - 1, y + bh - 1, NWP_CH_BR, role);
}

void nwp_clear(struct nwp_window *w, int role)
{
    nwp_fill(w, 0, 0, w->cols, w->rows, ' ', role);
}

/* ---------------- 한 줄 입력 ---------------- */

void nwp_le_init(struct nwp_lineedit *le, char *buf, int cap)
{
    le->buf = buf;
    le->cap = cap;
    le->len = 0;
    le->pos = 0;
    buf[0] = '\0';
}

void nwp_le_set(struct nwp_lineedit *le, const char *s)
{
    int n = (int)strlen(s);

    if (n > le->cap - 1)
        n = le->cap - 1;
    memcpy(le->buf, s, (size_t)n);
    le->buf[n] = '\0';
    le->len = n;
    le->pos = n;
}

static void leCut(struct nwp_lineedit *le, int from, int to)
{
    if (from >= to)
        return;
    memmove(le->buf + from, le->buf + to, (size_t)(le->len - to + 1));
    le->len -= to - from;
    le->pos = from;
}

static int leWordLeft(const struct nwp_lineedit *le)
{
    int p = le->pos;

    while (p > 0 && le->buf[p - 1] == ' ')
        p--;
    while (p > 0 && le->buf[p - 1] != ' ')
        p--;
    return p;
}

static int leWordRight(const struct nwp_lineedit *le)
{
    int p = le->pos;

    while (p < le->len && le->buf[p] == ' ')
        p++;
    while (p < le->len && le->buf[p] != ' ')
        p++;
    return p;
}

int nwp_le_key(struct nwp_lineedit *le, int key)
{
    int base = NWP_KEY_BASE(key);
    int ctrl = (key & NWP_MOD_CTRL) != 0;

    if (key & (NWP_MOD_ALT | NWP_MOD_SUPER))
        return 0;
    switch (base) {
    case NWP_KEY_LEFT:
        le->pos = ctrl ? leWordLeft(le) : (le->pos > 0 ? le->pos - 1 : 0);
        return 1;
    case NWP_KEY_RIGHT:
        le->pos = ctrl ? leWordRight(le) : (le->pos < le->len ? le->pos + 1 : le->len);
        return 1;
    case NWP_KEY_HOME:
    case NWP_KEY_CTRL('a'):
        le->pos = 0;
        return 1;
    case NWP_KEY_END:
    case NWP_KEY_CTRL('e'):
        le->pos = le->len;
        return 1;
    case 0x7F:
    case 0x08:
        if (ctrl)
            leCut(le, leWordLeft(le), le->pos);
        else if (le->pos > 0)
            leCut(le, le->pos - 1, le->pos);
        return 1;
    case NWP_KEY_CTRL('w'):
        leCut(le, leWordLeft(le), le->pos);
        return 1;
    case NWP_KEY_DEL:
        if (ctrl)
            leCut(le, le->pos, leWordRight(le));
        else if (le->pos < le->len)
            leCut(le, le->pos, le->pos + 1);
        return 1;
    case NWP_KEY_CTRL('u'):
        leCut(le, 0, le->pos);
        return 1;
    case NWP_KEY_CTRL('k'):
        le->buf[le->pos] = '\0';
        le->len = le->pos;
        return 1;
    default:
        break;
    }
    if (base >= 0x20 && base < 0x7F && !ctrl && le->len < le->cap - 1) {
        memmove(le->buf + le->pos + 1, le->buf + le->pos, (size_t)(le->len - le->pos + 1));
        le->buf[le->pos++] = (char)base;
        le->len++;
        return 1;
    }
    return 0;
}

void nwp_le_draw(struct nwp_window *w, const struct nwp_lineedit *le, int x, int y, int width, int secret, int cursor)
{
    int start = le->pos >= width ? le->pos - width + 1 : 0;
    int c;

    for (c = 0; c < width; c++) {
        int i = start + c;
        int ch = i < le->len ? (secret ? '*' : (unsigned char)le->buf[i]) : ' ';

        nwp_put_role(w, x + c, y, ch, cursor && i == le->pos ? NWP_ROLE_SELECTION : NWP_ROLE_WINDOW);
    }
}
