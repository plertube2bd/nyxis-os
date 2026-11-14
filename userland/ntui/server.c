/*
 * server.c - NWP 서버: 창 프로그램(클라이언트)과의 연결 (규약은 <nwp.h>, docs/NWP.md)
 *
 * 받는 곳: 채널 svc:/<사용자>/nwp (같은 사용자의 프로세스만 보낼 수 있다). 이 이름을 NWP_DISPLAY 로 정해 두면
 * ntui 가 띄운 프로그램과 그 자식들이 물려받는다.
 *   "nwp up <쿠키>"   + 위 파이프의 읽기 끝    클라이언트 연결의 반쪽
 *   "nwp down <쿠키>" + 아래 파이프의 쓰기 끝  나머지 반쪽 (둘 다 오면 연결됨)
 *   "nwp buf <쿠키> <번호>" + 공유 메모리      버퍼 (매핑해서 commit 때 읽는다)
 * 채널은 파이프보다 먼저 비운다: 클라이언트는 버퍼를 채널로 보낸 뒤에 파이프로 ATTACH 하므로, ATTACH 를 읽을 때는
 * 버퍼가 이미 와 있다.
 *
 * 창(서피스) = wm 의 창 하나 + 이 파일의 struct surface. 그리기는 commit 때 버퍼에서 복사해 둔 칸을 화면에 옮기는 것뿐.
 * 크기는 컴포지터가 정한다: 창의 안쪽 크기가 바뀌면(배치, 크기 조절, 전체 화면) CONFIGURE 를 보낸다.
 * 키는 활성 창의 클라이언트에게 KEY 로, 포커스가 바뀌면 FOCUS, 사용자가 닫으면(Alt+F4) CLOSE. 클라이언트가 응답하지
 * 않으면 두 번째 닫기 때 그 프로세스를 끝낸다.
 *
 * ntui 는 클라이언트 때문에 멈추지 않는다: 보낼 이벤트는 클라이언트마다 쌓아 두고 파이프에 자리가 있는 만큼만 쓴다.
 * 쌓인 것이 SERVER_OUT_MAX 를 넘으면(읽지 않는 클라이언트) 연결을 끊는다. 규약을 어기면 ERROR 를 보내고 끊는다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <nx64.h>

#include "ntui.h"

#define SERVER_CLIENTS   16
#define SERVER_SURFACES  4              /* 클라이언트 하나의 창 */
#define SERVER_BUFFERS   4
#define SERVER_IN_MAX    4096
#define SERVER_OUT_MAX   (64 * 1024)
#define PIPE_CAP         4096           /* NX_PIPE_CAPACITY */

struct client;

struct surface {
    struct client *c;                   /* NULL = 클라이언트가 사라졌다 (창은 곧 닫힌다) */
    unsigned id;
    struct window *win;
    struct nwp_cell *cells;             /* commit 때 버퍼에서 복사한 것 */
    int cols, rows;
    unsigned buffer;                    /* ATTACH 한 버퍼 */
    int bcols, brows;
    int confCols, confRows;             /* 마지막으로 보낸 CONFIGURE */
    int confState;
    int placeX, placeY;                 /* 마지막으로 보낸 PLACE */
    int placeCols, placeRows;
    int focused;                        /* 마지막으로 보낸 FOCUS */
    int inhibit;
    unsigned long closeAsked;           /* CLOSE 를 보낸 시각 (ms, 0 = 아님) */
};

struct buffer {
    unsigned id;
    int fd;
    const struct nwp_cell *map;
    size_t cells;
};

struct client {
    int used;
    unsigned long pid;
    char cookie[32];
    int up;                             /* 요청을 읽는다 */
    int down;                           /* 이벤트를 쓴다 */
    int hello;
    char app[16];
    struct buffer bufs[SERVER_BUFFERS];
    struct surface *surf[SERVER_SURFACES];
    unsigned char in[SERVER_IN_MAX];
    size_t inLen;
    unsigned char *out;
    size_t outLen;
    size_t outCap;
};

static struct client g_clients[SERVER_CLIENTS];
static long g_chan = -1;
static int g_chanFd = -1;
static int g_reload;
static char g_display[96];

static const struct app g_remoteApp;
static void disconnect(struct client *c, const char *why);

/* ---------------- 보내기 ---------------- */

static void flushClient(struct client *c)
{
    struct stat st;
    size_t room;
    ssize_t n;

    if (!c->used || c->down < 0 || c->outLen == 0)
        return;
    if (fstat(c->down, &st) < 0)
        return;
    room = st.st_size < PIPE_CAP ? (size_t)(PIPE_CAP - st.st_size) : 0;   /* 파이프에 남은 자리: 막히지 않는다 */
    if (room == 0)
        return;
    n = write(c->down, c->out, room < c->outLen ? room : c->outLen);
    if (n < 0) {
        if (errno != EINTR)
            disconnect(c, NULL);        /* 읽는 쪽이 없다 */
        return;
    }
    memmove(c->out, c->out + n, c->outLen - (size_t)n);
    c->outLen -= (size_t)n;
}

static void sendEv(struct client *c, unsigned type, unsigned surface, const void *data, size_t len)
{
    struct nwp_hdr h;
    size_t need = sizeof(h) + len;

    if (!c->used || c->down < 0)
        return;
    if (c->outLen + need > SERVER_OUT_MAX) {
        disconnect(c, "not reading events");
        return;
    }
    if (c->outLen + need > c->outCap) {
        size_t cap = c->outCap ? c->outCap * 2 : 1024;
        unsigned char *p;

        while (cap < c->outLen + need)
            cap *= 2;
        p = (unsigned char *)realloc(c->out, cap);
        if (p == NULL) {
            disconnect(c, NULL);
            return;
        }
        c->out = p;
        c->outCap = cap;
    }
    h.type = (unsigned short)type;
    h.len = (unsigned short)len;
    h.surface = surface;
    memcpy(c->out + c->outLen, &h, sizeof(h));
    if (len)
        memcpy(c->out + c->outLen + sizeof(h), data, len);
    c->outLen += need;
    flushClient(c);
}

static void sendTheme(struct client *c)
{
    struct nwp_theme t;
    int i;

    memset(&t, 0, sizeof(t));
    for (i = 0; i < NWP_ROLE_COUNT; i++) {
        int fg;
        int bg;
        unsigned attr;

        screenStyle((enum role)i, &fg, &bg, &attr);
        t.role[i][0] = (unsigned char)fg;
        t.role[i][1] = (unsigned char)bg;
        t.role[i][2] = (unsigned char)attr;
    }
    snprintf(t.name, sizeof(t.name), "%s", currentTheme->name);
    sendEv(c, NWP_EV_THEME, 0, &t, sizeof(t));
}

static void protocolError(struct client *c, const char *msg)
{
    sendEv(c, NWP_EV_ERROR, 0, msg, strlen(msg));
    flushClient(c);
    disconnect(c, NULL);
}

/* ---------------- 연결 ---------------- */

static void closeFd(int *fd)
{
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static void disconnect(struct client *c, const char *why)
{
    int i;

    if (!c->used)
        return;
    if (why != NULL) {
        char msg[96];

        snprintf(msg, sizeof(msg), "%s (pid %lu): %s", c->app[0] ? c->app : "client", c->pid, why);
        ntuiToast(msg);
    }
    for (i = 0; i < SERVER_SURFACES; i++) {
        struct surface *s = c->surf[i];

        if (s != NULL) {
            s->c = NULL;                /* 창은 다음 wmReap 이 닫는다 (그때 s 를 해제) */
            s->win->wantClose = 1;
            c->surf[i] = NULL;
        }
    }
    for (i = 0; i < SERVER_BUFFERS; i++)
        closeFd(&c->bufs[i].fd);
    closeFd(&c->up);
    closeFd(&c->down);
    free(c->out);
    memset(c, 0, sizeof(*c));
    c->up = c->down = -1;
}

static struct client *findCookie(const char *cookie, unsigned long pid)
{
    int i;

    for (i = 0; i < SERVER_CLIENTS; i++) {
        struct client *c = &g_clients[i];

        if (c->used && c->pid == pid && strcmp(c->cookie, cookie) == 0)
            return c;
    }
    return NULL;
}

/* 채널 메시지 하나 (fd 가 붙어 있을 수 있다: 쓰지 않으면 닫는다) */
static void channelMessage(const char *msg, unsigned long pid, int fd)
{
    char kind[8];
    char cookie[32];
    unsigned id = 0;
    struct client *c;
    int i;

    if (fd >= 0)
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);      /* 띄우는 프로그램에게 가지 않게 */
    if (fd < 0 || sscanf(msg, "nwp %7s %31s %u", kind, cookie, &id) < 2) {
        if (fd >= 0)
            (void)close(fd);
        return;
    }
    c = findCookie(cookie, pid);
    if (strcmp(kind, "up") == 0 && c == NULL) {
        for (i = 0; i < SERVER_CLIENTS && g_clients[i].used; i++)
            ;
        if (i == SERVER_CLIENTS) {
            ntuiToast("too many window programs");
            (void)close(fd);
            return;
        }
        c = &g_clients[i];
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->pid = pid;
        snprintf(c->cookie, sizeof(c->cookie), "%s", cookie);
        c->up = fd;
        c->down = -1;
        for (i = 0; i < SERVER_BUFFERS; i++)
            c->bufs[i].fd = -1;
        return;
    }
    if (strcmp(kind, "down") == 0 && c != NULL && c->down < 0) {
        c->down = fd;
        return;
    }
    if (strcmp(kind, "buf") == 0 && c != NULL && id != 0) {
        struct stat st;
        const struct nwp_cell *map;

        for (i = 0; i < SERVER_BUFFERS && c->bufs[i].fd >= 0; i++)
            ;
        map = (const struct nwp_cell *)nx64_shm_map(fd);
        if (i == SERVER_BUFFERS || map == NULL || fstat(fd, &st) < 0) {
            (void)close(fd);
            protocolError(c, "bad buffer (or too many buffers)");
            return;
        }
        c->bufs[i].id = id;
        c->bufs[i].fd = fd;
        c->bufs[i].map = map;
        c->bufs[i].cells = (size_t)st.st_size / sizeof(struct nwp_cell);
        return;
    }
    (void)close(fd);
}

/* ---------------- 창 ---------------- */

static void innerSize(const struct window *w, int *cols, int *rows)
{
    if (w->layout == LAYOUT_FULL) {
        *cols = screenCols;
        *rows = screenRows;
    } else {
        *cols = w->w - 2;
        *rows = w->h - 2;
    }
}

static int layoutState(const struct window *w)
{
    if (w->minimized)
        return NWP_STATE_MINIMIZED;
    if (w->layout == LAYOUT_FULL)
        return NWP_STATE_FULLSCREEN;
    if (w->layout == LAYOUT_MAX)
        return NWP_STATE_MAXIMIZED;
    return NWP_STATE_NORMAL;
}

static void configure(struct surface *s, int cols, int rows)
{
    struct nwp_configure cf;

    if (s->c == NULL || (cols == s->confCols && rows == s->confRows && layoutState(s->win) == s->confState))
        return;
    cf.cols = (unsigned short)(cols > 0 ? cols : 1);
    cf.rows = (unsigned short)(rows > 0 ? rows : 1);
    cf.state = (unsigned)layoutState(s->win);
    s->confCols = cols;
    s->confRows = rows;
    s->confState = (int)cf.state;
    sendEv(s->c, NWP_EV_CONFIGURE, s->id, &cf, sizeof(cf));
}

/* 창의 자리를 알린다 (바뀌었을 때만) */
static void place(struct surface *s)
{
    struct nwp_place pl;
    int x = s->win->layout == LAYOUT_FULL ? 0 : s->win->x;
    int y = s->win->layout == LAYOUT_FULL ? 0 : s->win->y;

    if (s->c == NULL || (x == s->placeX && y == s->placeY && screenCols == s->placeCols &&
                         wmAreaRows() == s->placeRows))
        return;
    pl.x = (short)x;
    pl.y = (short)y;
    pl.area_cols = (unsigned short)screenCols;
    pl.area_rows = (unsigned short)wmAreaRows();
    s->placeX = x;
    s->placeY = y;
    s->placeCols = screenCols;
    s->placeRows = wmAreaRows();
    sendEv(s->c, NWP_EV_PLACE, s->id, &pl, sizeof(pl));
}

/* 프로그램의 부탁으로 배치가 바뀌었다: 그리기를 기다리지 않고 바로 알린다 (최소화된 창은 그려지지 않는다) */
static void notifyLayout(struct surface *s)
{
    int cols;
    int rows;

    innerSize(s->win, &cols, &rows);
    configure(s, cols, rows);
    place(s);
}

static void remoteDraw(struct window *w, int x, int y, int cols, int rows, int active)
{
    struct surface *s = (struct surface *)w->data;
    int r;
    int c;

    if (s == NULL)
        return;
    configure(s, cols, rows);
    place(s);
    if (s->c != NULL && active != s->focused) {
        unsigned on = active ? 1U : 0U;

        s->focused = active;
        sendEv(s->c, NWP_EV_FOCUS, s->id, &on, sizeof(on));
    }
    if (s->cells == NULL) {
        screenText(x + 1, y, s->c != NULL ? "..." : "", ROLE_BORDER);
        return;
    }
    for (r = 0; r < rows && r < s->rows; r++) {
        for (c = 0; c < cols && c < s->cols; c++) {
            const struct nwp_cell *cell = &s->cells[r * s->cols + c];

            screenPutColor(x + c, y + r, cell->ch ? cell->ch : ' ', cell->fg, cell->bg, cell->attr);
        }
    }
}

static int remoteKey(struct window *w, int key)
{
    struct surface *s = (struct surface *)w->data;

    if (s != NULL && s->c != NULL)
        sendEv(s->c, NWP_EV_KEY, s->id, &key, sizeof(key));
    return 1;
}

/* 사용자가 닫았다: 클라이언트에게 부탁한다. 두 번째에도(3초 뒤) 남아 있으면 프로세스를 끝낸다 */
static void remoteRequestClose(struct window *w)
{
    struct surface *s = (struct surface *)w->data;
    unsigned long now = nx64_uptime_ms();

    if (s == NULL || s->c == NULL) {
        w->wantClose = 1;
        return;
    }
    if (s->closeAsked != 0 && now - s->closeAsked >= 3000UL) {
        char msg[80];

        snprintf(msg, sizeof(msg), "%s (pid %lu) did not close: stopped", s->c->app, s->c->pid);
        ntuiToast(msg);
        (void)nx64_signal_pid(s->c->pid, SIGKILL);
        disconnect(s->c, NULL);
        return;
    }
    if (s->closeAsked == 0)
        s->closeAsked = now;
    sendEv(s->c, NWP_EV_CLOSE, s->id, NULL, 0);
}

static void remoteClose(struct window *w)
{
    struct surface *s = (struct surface *)w->data;
    int i;

    if (s == NULL)
        return;
    if (s->c != NULL) {
        for (i = 0; i < SERVER_SURFACES; i++) {
            if (s->c->surf[i] == s)
                s->c->surf[i] = NULL;
        }
    }
    free(s->cells);
    free(s);
    w->data = NULL;
}

static const struct app g_remoteApp = {
    "remote", remoteDraw, remoteKey, NULL, NULL, NULL, remoteClose, remoteRequestClose
};

static struct surface *findSurface(struct client *c, unsigned id)
{
    int i;

    for (i = 0; i < SERVER_SURFACES; i++) {
        if (c->surf[i] != NULL && c->surf[i]->id == id)
            return c->surf[i];
    }
    return NULL;
}

static struct buffer *findBuffer(struct client *c, unsigned id)
{
    int i;

    for (i = 0; i < SERVER_BUFFERS; i++) {
        if (c->bufs[i].fd >= 0 && c->bufs[i].id == id)
            return &c->bufs[i];
    }
    return NULL;
}

static void createSurface(struct client *c, unsigned id, const struct nwp_create *cr)
{
    struct surface *s;
    struct window *w;
    char title[33];
    int slot;
    int cols;
    int rows;

    for (slot = 0; slot < SERVER_SURFACES && c->surf[slot] != NULL; slot++)
        ;
    if (id == 0 || findSurface(c, id) != NULL || slot == SERVER_SURFACES) {
        protocolError(c, "bad surface id (or too many windows)");
        return;
    }
    s = (struct surface *)calloc(1, sizeof(*s));
    if (s == NULL) {
        protocolError(c, "out of memory");
        return;
    }
    memcpy(title, cr->title, sizeof(cr->title));
    title[32] = '\0';
    w = wmOpen(title[0] ? title : c->app, &g_remoteApp, s,
               cr->cols ? cr->cols + 2 : screenCols * 3 / 4, cr->rows ? cr->rows + 2 : (screenRows - TASKBAR_ROWS) * 3 / 4);
    if (w == NULL) {
        free(s);
        ntuiToast("too many windows");
        sendEv(c, NWP_EV_CLOSE, id, NULL, 0);
        return;
    }
    s->c = c;
    s->id = id;
    s->win = w;
    s->confCols = s->confRows = -1;
    s->confState = -1;
    s->placeX = s->placeY = -1;
    s->focused = -1;
    c->surf[slot] = s;
    if (cr->state == NWP_STATE_MAXIMIZED)
        wmSetLayout(w, LAYOUT_MAX);
    else if (cr->state == NWP_STATE_FULLSCREEN)
        wmSetLayout(w, LAYOUT_FULL);
    innerSize(w, &cols, &rows);
    configure(s, cols, rows);
    place(s);
}

static void commit(struct client *c, struct surface *s)
{
    struct buffer *b = findBuffer(c, s->buffer);
    size_t n;

    if (b == NULL || s->bcols <= 0 || s->brows <= 0) {
        protocolError(c, "commit without a buffer");
        return;
    }
    n = (size_t)s->bcols * (size_t)s->brows;
    if (n > b->cells) {
        protocolError(c, "buffer too small");
        return;
    }
    if (s->cells == NULL || s->cols != s->bcols || s->rows != s->brows) {
        struct nwp_cell *cells = (struct nwp_cell *)malloc(n * sizeof(*cells));

        if (cells == NULL)
            return;
        free(s->cells);
        s->cells = cells;
        s->cols = s->bcols;
        s->rows = s->brows;
    }
    memcpy(s->cells, b->map, n * sizeof(*s->cells));
    sendEv(c, NWP_EV_FRAME, s->id, NULL, 0);
}

/* 요청 하나. 다시 그려야 하면 1 */
static int request(struct client *c, const struct nwp_hdr *h, const unsigned char *p)
{
    struct surface *s = h->surface ? findSurface(c, h->surface) : NULL;
    unsigned v = 0;

    if (h->len >= sizeof(v))
        memcpy(&v, p, sizeof(v));
    if (!c->hello && h->type != NWP_REQ_HELLO) {
        protocolError(c, "expected HELLO");
        return 0;
    }
    switch (h->type) {
    case NWP_REQ_HELLO: {
        struct nwp_hello hello;
        struct nwp_welcome wl;

        if (h->len < sizeof(hello) || c->hello) {
            protocolError(c, "bad HELLO");
            return 0;
        }
        memcpy(&hello, p, sizeof(hello));
        if (hello.version != NWP_VERSION) {
            protocolError(c, "unsupported protocol version");
            return 0;
        }
        memcpy(c->app, hello.app_id, sizeof(c->app));
        c->app[sizeof(c->app) - 1] = '\0';
        c->hello = 1;
        memset(&wl, 0, sizeof(wl));
        wl.version = NWP_VERSION;
        wl.flags = inputDisplayMode() ? NWP_WELCOME_DISPLAY : 0U;
        wl.screen_cols = (unsigned short)screenCols;
        wl.screen_rows = (unsigned short)screenRows;
        sendEv(c, NWP_EV_WELCOME, 0, &wl, sizeof(wl));
        sendTheme(c);
        return 0;
    }
    case NWP_REQ_CREATE: {
        struct nwp_create cr;

        if (h->len < sizeof(cr)) {
            protocolError(c, "bad CREATE");
            return 0;
        }
        memcpy(&cr, p, sizeof(cr));
        createSurface(c, h->surface, &cr);
        return 1;
    }
    case NWP_REQ_BUFFER_FREE: {
        struct buffer *b = findBuffer(c, v);

        if (b != NULL) {
            closeFd(&b->fd);
            b->map = NULL;
        }
        return 0;
    }
    case NWP_REQ_RELOAD:
        g_reload = 1;
        return 0;
    case NWP_REQ_SYNC:
        /* 앞의 요청은 다 처리했다 (그 CONFIGURE/PLACE 는 이미 쌓였다): 같은 번호로 답한다 */
        sendEv(c, NWP_EV_DONE, 0, &v, sizeof(v));
        return 0;
    default:
        break;
    }
    if (s == NULL) {
        protocolError(c, "unknown surface");
        return 0;
    }
    switch (h->type) {
    case NWP_REQ_TITLE:
        if (h->len > 0) {
            size_t n = h->len < sizeof(s->win->title) - 1 ? h->len : sizeof(s->win->title) - 1;

            memcpy(s->win->title, p, n);
            s->win->title[n] = '\0';
        }
        return 1;
    case NWP_REQ_ATTACH: {
        struct nwp_attach a;

        if (h->len < sizeof(a)) {
            protocolError(c, "bad ATTACH");
            return 0;
        }
        memcpy(&a, p, sizeof(a));
        if (a.format != NWP_FORMAT_CELLS || findBuffer(c, a.buffer) == NULL) {
            protocolError(c, "ATTACH: unknown buffer or format");
            return 0;
        }
        s->buffer = a.buffer;
        s->bcols = a.cols;
        s->brows = a.rows;
        return 0;
    }
    case NWP_REQ_COMMIT:
        commit(c, s);
        return 1;
    case NWP_REQ_DESTROY:
        s->win->wantClose = 1;
        return 1;
    case NWP_REQ_STATE: {
        int idx = wmIndexOf(s->win);

        if (v == NWP_STATE_MINIMIZED) {
            wmMinimize(idx);
        } else if (v == NWP_STATE_NORMAL && s->win->minimized) {
            wmSetActive(idx);           /* 최소화를 풀기만 한다 (최대화였으면 그대로: xterm 의 de-iconify) */
        } else {
            if (s->win->minimized)
                wmSetActive(idx);       /* 되살린다 (xterm 의 de-iconify) */
            if (v == NWP_STATE_FULLSCREEN && s->win->layout != LAYOUT_FULL)
                s->win->prevLayout = s->win->layout;   /* F11 이 돌아갈 곳 */
            wmSetLayout(s->win, v == NWP_STATE_MAXIMIZED ? LAYOUT_MAX : v == NWP_STATE_FULLSCREEN ? LAYOUT_FULL
                                                                                                    : LAYOUT_NORMAL);
        }
        notifyLayout(s);
        return 1;
    }
    case NWP_REQ_MOVE: {
        struct nwp_move m;

        if (h->len < sizeof(m)) {
            protocolError(c, "bad MOVE");
            return 0;
        }
        memcpy(&m, p, sizeof(m));
        wmPlace(s->win, m.x < 0 ? 0 : m.x, m.y < 0 ? 0 : m.y, -1, -1);
        notifyLayout(s);
        return 1;
    }
    case NWP_REQ_RESIZE: {
        struct nwp_resize rs;

        if (h->len < sizeof(rs)) {
            protocolError(c, "bad RESIZE");
            return 0;
        }
        memcpy(&rs, p, sizeof(rs));
        wmPlace(s->win, -1, -1, rs.cols ? rs.cols + 2 : -1, rs.rows ? rs.rows + 2 : -1);
        notifyLayout(s);
        return 1;
    }
    case NWP_REQ_INHIBIT:
        s->inhibit = v != 0;
        return 0;
    default:
        return 0;                       /* 모르는 요청 (새 판의 클라이언트): 넘긴다 */
    }
}

static int readClient(struct client *c)
{
    ssize_t n = read(c->up, c->in + c->inLen, sizeof(c->in) - c->inLen);
    int dirty = 0;

    if (n < 0 && errno == EINTR)
        return 0;
    if (n <= 0) {
        disconnect(c, NULL);            /* 클라이언트가 끝났다 */
        return 1;
    }
    c->inLen += (size_t)n;
    while (c->used && c->inLen >= sizeof(struct nwp_hdr)) {
        struct nwp_hdr h;
        size_t total;

        memcpy(&h, c->in, sizeof(h));
        if (h.len > NWP_MSG_MAX) {
            protocolError(c, "message too long");
            return 1;
        }
        total = sizeof(h) + h.len;
        if (c->inLen < total)
            break;
        dirty |= request(c, &h, c->in + sizeof(h));
        if (!c->used)
            return 1;
        memmove(c->in, c->in + total, c->inLen - total);
        c->inLen -= total;
    }
    return dirty;
}

/* ---------------- 이벤트 루프 ---------------- */

int serverInit(void)
{
    snprintf(g_display, sizeof(g_display), "svc:/%s/nwp", username);
    g_chan = nx64_service_create(g_display, 0);
    if (g_chan < 0)
        return -1;
    g_chanFd = nx64_service_fd(g_chan);
    if (g_chanFd < 0)
        return -1;
    setenv("NWP_DISPLAY", g_display, 1);
    return 0;
}

int serverPollFds(struct pollfd *fds, int max)
{
    int n = 0;
    int i;

    if (g_chanFd >= 0 && n < max) {
        fds[n].fd = g_chanFd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }
    for (i = 0; i < SERVER_CLIENTS && n < max; i++) {
        struct client *c = &g_clients[i];

        if (!c->used || c->down < 0)
            continue;                   /* 반쪽만 온 연결은 아직 읽지 않는다 */
        fds[n].fd = c->up;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }
    return n;
}

int serverPollDone(const struct pollfd *fds, int n)
{
    int dirty = 0;
    int k;
    int i;

    /* 채널을 먼저 (버퍼가 ATTACH 보다 먼저 와 있게) */
    for (k = 0; k < n; k++) {
        if (fds[k].fd != g_chanFd || !(fds[k].revents & POLLIN))
            continue;
        for (;;) {
            char msg[256 + 1];                  /* NX_CHAN_MSG_MAX */
            struct nx64_sender from;
            int fd = -1;
            long r = nx64_service_receive_fd(g_chan, msg, sizeof(msg) - 1, &from, 0, &fd);

            if (r < 0)
                break;
            msg[r] = '\0';
            channelMessage(msg, from.pid, fd);
        }
    }
    for (k = 0; k < n; k++) {
        if (fds[k].fd == g_chanFd || !fds[k].revents)
            continue;
        for (i = 0; i < SERVER_CLIENTS; i++) {
            struct client *c = &g_clients[i];

            if (c->used && c->up == fds[k].fd) {
                dirty |= readClient(c);
                break;
            }
        }
    }
    return dirty;
}

int serverFlush(void)
{
    int i;

    for (i = 0; i < SERVER_CLIENTS; i++)
        flushClient(&g_clients[i]);
    return 0;
}

void serverThemeChanged(void)
{
    int i;

    for (i = 0; i < SERVER_CLIENTS; i++) {
        if (g_clients[i].used && g_clients[i].hello)
            sendTheme(&g_clients[i]);
    }
}

static struct surface *activeSurface(void)
{
    struct window *w = wmActive();

    if (w == NULL || w->app != &g_remoteApp)
        return NULL;
    return (struct surface *)w->data;
}

const char *serverActiveAppId(void)
{
    struct surface *s = activeSurface();

    return (s != NULL && s->c != NULL) ? s->c->app : "";
}

int serverInhibited(void)
{
    struct surface *s = activeSurface();

    return s != NULL && s->c != NULL && s->inhibit;
}

int serverSignalActive(int sig)
{
    struct surface *s = activeSurface();
    unsigned v = (unsigned)sig;

    if (s == NULL || s->c == NULL)
        return 0;
    sendEv(s->c, NWP_EV_SIGNAL, s->id, &v, sizeof(v));
    return 1;
}

int serverActivateApp(const char *app_id)
{
    int i;
    int k;

    for (i = 0; i < SERVER_CLIENTS; i++) {
        struct client *c = &g_clients[i];

        if (!c->used || strcmp(c->app, app_id) != 0)
            continue;
        for (k = 0; k < SERVER_SURFACES; k++) {
            if (c->surf[k] != NULL) {
                wmSetActive(wmIndexOf(c->surf[k]->win));
                return 1;
            }
        }
    }
    return 0;
}

int serverConsumeReload(void)
{
    int r = g_reload;

    g_reload = 0;
    return r;
}

void serverShutdown(void)
{
    int i;

    for (i = 0; i < SERVER_CLIENTS; i++) {
        struct client *c = &g_clients[i];
        int k;

        if (!c->used)
            continue;
        for (k = 0; k < SERVER_SURFACES; k++) {
            if (c->surf[k] != NULL)
                sendEv(c, NWP_EV_CLOSE, c->surf[k]->id, NULL, 0);
        }
        flushClient(c);
        disconnect(c, NULL);            /* 연결이 끊기면 클라이언트는 끝난다 (터미널의 셸은 SIGHUP) */
    }
    if (g_chan >= 0) {
        (void)close(g_chanFd);
        g_chan = -1;
        g_chanFd = -1;
    }
}
