/*
 * nx64_tui.c - nx64api TUI (규칙은 nx64.h). 구조는 ntui 의 term.c/screen.c 와 같다:
 *   raw 모드(VMIN=0, VTIME=1: 100ms 마다 깨어나 ESC 단독 키를 구분), 셀 버퍼 두 장(다음/지금), 바뀐 칸만 내보내기.
 */
#include <nx64.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>

#define NX64_OUT_MAX 4096

struct nx64_cell {
    char ch;
    nx64_style s;
};

/* nyx 테마와 같은 색 (ntui theme.c) */
const nx64_style NX64_DEFAULT  = { 250, 234, 0 };
const nx64_style NX64_FRAME    = {  60, 234, 0 };
const nx64_style NX64_ACCENT   = { 231,  54, NX64_BOLD };
const nx64_style NX64_SELECTED = { 231,  23, 0 };
const nx64_style NX64_DIM      = { 244, 234, 0 };

static int g_tty = -1;
static int g_open_tty;                  /* dev:/tty 를 직접 열었으면 1 (닫아야 한다) */
static struct termios g_orig;
static int g_cols = 80;
static int g_rows = 24;
static struct nx64_cell *g_back;
static struct nx64_cell *g_front;
static char g_out[NX64_OUT_MAX];
static size_t g_out_len;
static int g_atexit;

/* 창 조작 질의의 답을 기다리다가 함께 읽은 다른 바이트 (그사이에 친 키): read_byte 가 먼저 돌려준다 */
static unsigned char g_pend[256];
static size_t g_pend_len;

nx64_style nx64_style_make(int fg, int bg, int attr)
{
    nx64_style s;

    s.fg = (unsigned char)fg;
    s.bg = (unsigned char)bg;
    s.attr = (unsigned char)attr;
    return s;
}

/* ---------------- 출력 ---------------- */

static void out_flush(void)
{
    size_t off = 0;

    while (g_tty >= 0 && off < g_out_len) {
        ssize_t n = write(g_tty, g_out + off, g_out_len - off);

        if (n <= 0)
            break;
        off += (size_t)n;
    }
    g_out_len = 0;
}

static void out_str(const char *s, size_t n)
{
    if (g_out_len + n > sizeof(g_out))
        out_flush();
    if (n > sizeof(g_out))
        n = sizeof(g_out);
    memcpy(g_out + g_out_len, s, n);
    g_out_len += n;
}

static void out_cstr(const char *s)
{
    out_str(s, strlen(s));
}

/* ---------------- 시작 / 끝 ---------------- */

static void at_exit_end(void)
{
    nx64_tui_end();
}

int nx64_tui_begin(void)
{
    struct termios t;
    struct winsize ws;
    size_t n;
    int fd;

    if (g_back != NULL)
        return 0;
    fd = open("dev:/tty", O_RDWR);
    g_open_tty = fd >= 0;
    if (fd < 0)
        fd = 0;
    if (tcgetattr(fd, &g_orig) < 0)
        goto fail;
    t = g_orig;
    cfmakeraw(&t);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSAFLUSH, &t) < 0)
        goto fail;
    if (g_open_tty)
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    g_tty = fd;

    g_cols = 80;
    g_rows = 24;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        g_cols = ws.ws_col;
        g_rows = ws.ws_row;
    }
    n = (size_t)g_cols * (size_t)g_rows;
    g_back = (struct nx64_cell *)calloc(n, sizeof(struct nx64_cell));
    g_front = (struct nx64_cell *)calloc(n, sizeof(struct nx64_cell));
    if (g_back == NULL || g_front == NULL) {
        nx64_tui_end();
        return -1;
    }
    if (!g_atexit) {
        g_atexit = 1;
        atexit(at_exit_end);
    }
    out_cstr("\033[?25l\033[0m\033[2J");
    nx64_tui_clear(NX64_DEFAULT);
    nx64_tui_invalidate();
    return 0;
fail:
    if (g_open_tty)
        close(fd);
    g_open_tty = 0;
    return -1;
}

void nx64_tui_end(void)
{
    if (g_tty < 0)
        return;
    out_cstr("\033[0m\033[2J\033[H\033[?25h");
    out_flush();
    (void)tcsetattr(g_tty, TCSAFLUSH, &g_orig);
    if (g_open_tty)
        close(g_tty);
    g_tty = -1;
    g_open_tty = 0;
    free(g_back);
    free(g_front);
    g_back = g_front = NULL;
}

int nx64_tui_cols(void)
{
    return g_cols;
}

int nx64_tui_rows(void)
{
    return g_rows;
}

/* ---------------- 그리기 ---------------- */

void nx64_tui_put(int x, int y, int ch, nx64_style s)
{
    struct nx64_cell *c;

    if (g_back == NULL || x < 0 || y < 0 || x >= g_cols || y >= g_rows)
        return;
    c = &g_back[y * g_cols + x];
    c->ch = (char)((ch >= 0x20 && ch <= 0x7E) ? ch : '?');
    c->s = s;
}

int nx64_tui_text(int x, int y, const char *text, nx64_style s)
{
    if (text == NULL)
        return x;
    for (; *text != '\0'; text++, x++)
        nx64_tui_put(x, y, (unsigned char)*text, s);
    return x;
}

int nx64_tui_textf(int x, int y, nx64_style s, const char *fmt, ...)
{
    char buf[257];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return nx64_tui_text(x, y, buf, s);
}

void nx64_tui_fill(int x, int y, int w, int h, int ch, nx64_style s)
{
    int i;
    int j;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++)
            nx64_tui_put(x + i, y + j, ch, s);
    }
}

void nx64_tui_clear(nx64_style s)
{
    nx64_tui_fill(0, 0, g_cols, g_rows, ' ', s);
}

void nx64_tui_box(int x, int y, int w, int h, const char *title, nx64_style s)
{
    int i;

    if (w < 2 || h < 2)
        return;
    for (i = 1; i < w - 1; i++) {
        nx64_tui_put(x + i, y, '-', s);
        nx64_tui_put(x + i, y + h - 1, '-', s);
    }
    for (i = 1; i < h - 1; i++) {
        nx64_tui_put(x, y + i, '|', s);
        nx64_tui_put(x + w - 1, y + i, '|', s);
    }
    nx64_tui_put(x, y, '+', s);
    nx64_tui_put(x + w - 1, y, '+', s);
    nx64_tui_put(x, y + h - 1, '+', s);
    nx64_tui_put(x + w - 1, y + h - 1, '+', s);
    if (title != NULL && *title != '\0' && w > 6) {
        char t[256];
        size_t max = (size_t)(w - 6);
        size_t n = strlen(title);

        if (n > max)
            n = max;
        if (n > sizeof(t) - 1)
            n = sizeof(t) - 1;
        memcpy(t, title, n);
        t[n] = '\0';
        nx64_tui_put(x + 2, y, ' ', s);
        i = nx64_tui_text(x + 3, y, t, s);
        nx64_tui_put(i, y, ' ', s);
    }
}

void nx64_tui_bar(int x, int y, int w, unsigned long value, unsigned long max, nx64_style on, nx64_style off)
{
    int filled = 0;
    int i;

    if (w <= 0)
        return;
    if (max > 0) {
        if (value > max)
            value = max;
        filled = (int)((double)value / (double)max * (double)w + 0.5);   /* 칸은 바탕색으로 채운다 */
    }
    for (i = 0; i < w; i++)
        nx64_tui_put(x + i, y, ' ', i < filled ? on : off);
}

/* ---------------- 내보내기 ---------------- */

void nx64_tui_invalidate(void)
{
    size_t i;
    size_t n = (size_t)g_cols * (size_t)g_rows;

    if (g_front == NULL)
        return;
    for (i = 0; i < n; i++) {
        g_front[i].ch = 0;              /* 그릴 수 있는 글자가 아니므로 늘 다르다 */
        g_front[i].s.attr = 0xFF;
    }
}

static int same_style(nx64_style a, nx64_style b)
{
    return a.fg == b.fg && a.bg == b.bg && a.attr == b.attr;
}

static void send_style(nx64_style s)
{
    char seq[48];

    sprintf(seq, "\033[0%s%s%s;38;5;%u;48;5;%um",
            (s.attr & NX64_BOLD) ? ";1" : "", (s.attr & NX64_UNDERLINE) ? ";4" : "",
            (s.attr & NX64_REVERSE) ? ";7" : "", (unsigned)s.fg, (unsigned)s.bg);
    out_cstr(seq);
}

void nx64_tui_flush(void)
{
    char seq[24];
    nx64_style cur;
    int have_style = 0;
    int cur_x = -1;
    int cur_y = -1;
    int x;
    int y;

    if (g_back == NULL)
        return;
    cur = NX64_DEFAULT;
    for (y = 0; y < g_rows; y++) {
        for (x = 0; x < g_cols; x++) {
            struct nx64_cell *b = &g_back[y * g_cols + x];
            struct nx64_cell *f = &g_front[y * g_cols + x];

            if (b->ch == f->ch && same_style(b->s, f->s))
                continue;
            if (x != cur_x || y != cur_y) {
                sprintf(seq, "\033[%d;%dH", y + 1, x + 1);
                out_cstr(seq);
            }
            if (!have_style || !same_style(b->s, cur)) {
                send_style(b->s);
                cur = b->s;
                have_style = 1;
            }
            out_str(&b->ch, 1);
            *f = *b;
            cur_x = x + 1;
            cur_y = y;
            if (cur_x >= g_cols)
                cur_x = -1;             /* 줄 끝: 다음 글자에서 줄이 바뀌므로 위치를 모른다고 본다 */
        }
    }
    out_flush();
}

/* ---------------- 입력 ---------------- */

/* 100ms 안에 1 바이트. 시간 초과 -1, 오류 -2 */
static int read_byte(void)
{
    unsigned char c;
    ssize_t n;

    if (g_pend_len > 0) {
        c = g_pend[0];
        memmove(g_pend, g_pend + 1, --g_pend_len);
        return c;
    }
    n = read(g_tty, &c, 1);

    if (n == 1)
        return c;
    return n == 0 ? -1 : -2;
}

/* 터미널 크기가 바뀌었으면 셀 버퍼를 새 크기로 (내용은 지운다: 다시 그려야 한다). 바뀌었으면 1 */
static int check_resize(void)
{
    struct winsize ws;
    struct nx64_cell *b;
    struct nx64_cell *f;
    size_t n;

    if (g_tty < 0 || ioctl(g_tty, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 || ws.ws_row == 0)
        return 0;
    if (ws.ws_col == g_cols && ws.ws_row == g_rows)
        return 0;
    n = (size_t)ws.ws_col * (size_t)ws.ws_row;
    b = (struct nx64_cell *)calloc(n, sizeof(struct nx64_cell));
    f = (struct nx64_cell *)calloc(n, sizeof(struct nx64_cell));
    if (b == NULL || f == NULL) {
        free(b);
        free(f);
        return 0;
    }
    free(g_back);
    free(g_front);
    g_back = b;
    g_front = f;
    g_cols = ws.ws_col;
    g_rows = ws.ws_row;
    out_cstr("\033[0m\033[2J");
    nx64_tui_clear(NX64_DEFAULT);
    nx64_tui_invalidate();
    return 1;
}

int nx64_tui_key(int timeout_ms)
{
    int c;
    int a;
    int b;
    int tries = timeout_ms < 0 ? -1 : (timeout_ms + 99) / 100;

    if (g_tty < 0)
        return -1;
    for (;;) {
        if (check_resize())
            return NX64_KEY_RESIZE;
        c = read_byte();
        if (c != -1)
            break;
        if (tries >= 0 && --tries <= 0)
            return NX64_KEY_NONE;
    }
    if (c < 0)
        return -1;
    if (c != 27)
        return c;
    if ((a = read_byte()) < 0 || a != '[')
        return NX64_KEY_ESC;
    if ((b = read_byte()) < 0)
        return NX64_KEY_ESC;
    switch (b) {
    case 'A': return NX64_KEY_UP;
    case 'B': return NX64_KEY_DOWN;
    case 'C': return NX64_KEY_RIGHT;
    case 'D': return NX64_KEY_LEFT;
    case 'H': return NX64_KEY_HOME;
    case 'F': return NX64_KEY_END;
    case '1': case '3': case '4': case '5': case '6': case '7': case '8':
        if (read_byte() != '~')
            return NX64_KEY_ESC;
        switch (b) {
        case '1': case '7': return NX64_KEY_HOME;
        case '4': case '8': return NX64_KEY_END;
        case '3': return NX64_KEY_DEL;
        case '5': return NX64_KEY_PGUP;
        default:  return NX64_KEY_PGDN;
        }
    default:
        return NX64_KEY_ESC;
    }
}

/* ---------------- 이벤트 루프 ---------------- */

int nx64_tui_run(const struct nx64_app *app, void *ctx)
{
    int dirty = 1;

    if (app == NULL || nx64_tui_begin() < 0)
        return -1;
    for (;;) {
        int k;

        if (dirty) {
            if (app->draw)
                app->draw(ctx);
            nx64_tui_flush();
            dirty = 0;
        }
        k = nx64_tui_key(app->tick_ms > 0 ? app->tick_ms : -1);
        if (k == -1)
            break;                      /* 입력이 끝났다 */
        if (k == NX64_KEY_RESIZE) {
            dirty = 1;                  /* 셀 버퍼는 이미 새 크기다 */
            continue;
        }
        if (k == NX64_KEY_NONE) {
            if (app->tick && app->tick(ctx))
                dirty = 1;
            continue;
        }
        if (app->key && !app->key(ctx, k))
            break;
        dirty = 1;
    }
    nx64_tui_end();
    return 0;
}

/* ---------------- ntui 창 (nterm 에 xterm 창 조작 시퀀스) ---------------- */

static int g_wfd = -1;                  /* TUI 밖에서 쓰는 터미널 (dev:/tty, close-on-exec) */
static int g_present = -1;              /* nx64_ntui_present 의 답 (-1 = 아직 묻지 않음) */
static int g_keep;                      /* nx64_win_keep: 끝날 때 되돌리지 않는다 */
static int g_saved;                     /* g_orig_win 에 처음 창을 적어 두었다 */
static struct nx64_win g_orig_win;

/* 창 조작에 쓸 터미널 fd */
static int win_fd(void)
{
    if (g_tty >= 0)
        return g_tty;
    if (g_wfd < 0) {
        g_wfd = open("dev:/tty", O_RDWR);
        if (g_wfd >= 0)
            (void)fcntl(g_wfd, F_SETFD, FD_CLOEXEC);
    }
    return g_wfd;
}

static int win_write(const char *seq)
{
    int fd = win_fd();
    size_t n = strlen(seq);

    if (fd < 0)
        return -1;
    if (fd == g_tty)
        out_flush();                    /* 그리던 것 뒤에 */
    return write(fd, seq, n) == (ssize_t)n ? 0 : -1;
}

static void pend(const char *p, size_t n)
{
    if (n > sizeof(g_pend) - g_pend_len)
        n = sizeof(g_pend) - g_pend_len;
    memcpy(g_pend + g_pend_len, p, n);
    g_pend_len += n;
}

static long now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/*
 * query 를 보내고 prefix ... term 꼴의 답을 기다린다 (최대 500ms). 답의 가운데를 body 에.
 * 그사이에 온 다른 바이트(키)는 g_pend 로 돌려 둔다. TUI 밖이면 잠시 raw 모드로 바꾼다. 성공 0
 */
static int win_ask(const char *query, const char *prefix, const char *term, char *body, size_t cap)
{
    int fd = win_fd();
    struct termios saved;
    struct termios t;
    int raw_changed = 0;
    char got[256];
    size_t len = 0;
    long deadline;
    int result = -1;

    if (fd < 0)
        return -1;
    if (fd != g_tty) {
        if (tcgetattr(fd, &saved) < 0)
            return -1;
        t = saved;
        t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 1;
        if (tcsetattr(fd, TCSANOW, &t) < 0)
            return -1;
        raw_changed = 1;
    }
    if (win_write(query) < 0)
        goto out;
    deadline = now_ms() + 500;
    while (now_ms() < deadline && len < sizeof(got) - 1) {
        unsigned char c;
        char *start;
        char *end;

        if (read(fd, &c, 1) != 1)
            continue;
        got[len++] = (char)c;
        got[len] = '\0';
        start = strstr(got, prefix);
        if (start == NULL)
            continue;
        end = strstr(start + strlen(prefix), term);
        if (end == NULL)
            continue;
        {
            size_t blen = (size_t)(end - (start + strlen(prefix)));
            size_t after = (size_t)(end + strlen(term) - got);

            if (blen >= cap)
                blen = cap - 1;
            memcpy(body, start + strlen(prefix), blen);
            body[blen] = '\0';
            pend(got, (size_t)(start - got));            /* 답 앞뒤의 키는 돌려 둔다 */
            pend(got + after, len - after);
            len = 0;
            result = 0;
            break;
        }
    }
    if (result < 0)
        pend(got, len);
out:
    if (raw_changed)
        (void)tcsetattr(fd, TCSANOW, &saved);
    return result;
}

int nx64_ntui_present(void)
{
    const char *d = getenv("NWP_DISPLAY");
    char body[64];

    if (g_present >= 0)
        return g_present;
    g_present = 0;
    /* ntui 가 띄운 것이 아니면 묻지 않는다 (다른 터미널에 모르는 시퀀스를 보내지 않게) */
    if (d == NULL || d[0] == '\0' || win_fd() < 0)
        return 0;
    if (win_ask("\033[>0q", "\033P>|", "\033\\", body, sizeof(body)) == 0 && strncmp(body, "nterm", 5) == 0)
        g_present = 1;
    return g_present;
}

void nx64_ntui_require(const char *prog)
{
    if (nx64_ntui_present())
        return;
    nx64_tui_end();
    fprintf(stderr, "%s: this program runs only in an ntui terminal window (open one with Super+Enter)\n",
            prog != NULL ? prog : "program");
    exit(1);
}

static int not_present(void)
{
    if (nx64_ntui_present())
        return 0;
    errno = ENOTSUP;
    return 1;
}

/* CSI code t 를 묻고 CSI want ; a ; b t 를 받는다 */
static int win_query(int code, int want, int *a, int *b)
{
    char q[16];
    char prefix[8];
    char body[32];
    char *semi;

    sprintf(q, "\033[%dt", code);
    sprintf(prefix, "\033[%d;", want);
    if (win_ask(q, prefix, "t", body, sizeof(body)) < 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    *a = atoi(body);
    semi = strchr(body, ';');
    *b = semi != NULL ? atoi(semi + 1) : 0;
    return 0;
}

int nx64_win_info(struct nx64_win *out)
{
    int a;
    int b;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (not_present())
        return -1;
    memset(out, 0, sizeof(*out));
    /* 11 은 답의 첫 숫자가 1(보통) 또는 2(최소화)라 둘 다 받는다: 접두어를 "ESC [" 로 하고 따로 나눈다 */
    {
        char body[32];
        char *semi;

        if (win_ask("\033[11t", "\033[", "t", body, sizeof(body)) < 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        semi = strchr(body, ';');
        out->state = semi != NULL ? atoi(semi + 1) : (atoi(body) == 2 ? NX64_WIN_MINIMIZED : NX64_WIN_NORMAL);
    }
    if (win_query(13, 3, &a, &b) < 0)
        return -1;
    out->x = a;
    out->y = b;
    if (win_query(18, 8, &a, &b) < 0)
        return -1;
    out->rows = a;
    out->cols = b;
    if (win_query(19, 9, &a, &b) < 0)
        return -1;
    out->area_rows = a;
    out->area_cols = b;
    return 0;
}

/* 끝날 때 되돌린다 */
static void win_restore(void)
{
    char seq[48];

    if (!g_saved || g_keep)
        return;
    g_saved = 0;
    (void)win_write("\033[1t");         /* 최소화돼 있으면 되살린다 */
    switch (g_orig_win.state) {
    case NX64_WIN_MAXIMIZED:
        (void)win_write("\033[9;1t");
        break;
    case NX64_WIN_FULLSCREEN:
        (void)win_write("\033[10;1t");
        break;
    case NX64_WIN_MINIMIZED:
        (void)win_write("\033[2t");
        break;
    default:
        (void)win_write("\033[9;0t");
        sprintf(seq, "\033[8;%d;%dt\033[3;%d;%dt", g_orig_win.rows, g_orig_win.cols, g_orig_win.x, g_orig_win.y);
        (void)win_write(seq);
        break;
    }
}

/* 처음 바꿀 때 지금 창을 적어 둔다 */
static int win_change(void)
{
    if (not_present())
        return -1;
    if (!g_saved && !g_keep && nx64_win_info(&g_orig_win) == 0) {
        g_saved = 1;
        atexit(win_restore);
    }
    return 0;
}

void nx64_win_keep(void)
{
    g_keep = 1;
}

int nx64_win_resize(int cols, int rows)
{
    char seq[32];

    if (cols < 0 || rows < 0 || cols > 1000 || rows > 1000) {
        errno = EINVAL;
        return -1;
    }
    if (win_change() < 0)
        return -1;
    sprintf(seq, "\033[8;%d;%dt", rows, cols);
    return win_write(seq);
}

int nx64_win_move(int x, int y)
{
    char seq[32];

    if (x < 0 || y < 0 || x > 1000 || y > 1000) {
        errno = EINVAL;
        return -1;
    }
    if (win_change() < 0)
        return -1;
    sprintf(seq, "\033[3;%d;%dt", x, y);
    return win_write(seq);
}

int nx64_win_center(void)
{
    struct nx64_win w;
    int x;
    int y;

    if (nx64_win_info(&w) < 0)
        return -1;
    x = (w.area_cols - (w.cols + 2)) / 2;
    y = (w.area_rows - (w.rows + 2)) / 2;
    return nx64_win_move(x > 0 ? x : 0, y > 0 ? y : 0);
}

int nx64_win_set_state(int state)
{
    const char *seq;

    switch (state) {
    case NX64_WIN_NORMAL:     seq = "\033[1t\033[9;0t"; break;
    case NX64_WIN_MAXIMIZED:  seq = "\033[9;1t"; break;
    case NX64_WIN_FULLSCREEN: seq = "\033[10;1t"; break;
    case NX64_WIN_MINIMIZED:  seq = "\033[2t"; break;
    default:
        errno = EINVAL;
        return -1;
    }
    if (win_change() < 0)
        return -1;
    return win_write(seq);
}

int nx64_win_title(const char *title)
{
    char seq[48];
    size_t i;

    if (title == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (not_present())
        return -1;
    strcpy(seq, "\033]2;");
    for (i = 0; title[i] != '\0' && i < 31; i++)
        seq[4 + i] = (unsigned char)title[i] >= 0x20 && (unsigned char)title[i] < 0x7F ? title[i] : '?';
    seq[4 + i] = '\007';
    seq[5 + i] = '\0';
    return win_write(seq);
}
