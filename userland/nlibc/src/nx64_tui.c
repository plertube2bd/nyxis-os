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
    ssize_t n = read(g_tty, &c, 1);

    if (n == 1)
        return c;
    return n == 0 ? -1 : -2;
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
