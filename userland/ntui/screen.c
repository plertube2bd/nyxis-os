/*
 * screen.c - 화면: 셀 버퍼 두 벌(그릴 것 / 화면에 있는 것)과 바뀐 칸만 그리기
 *
 * 그리는 함수는 g_back 에만 쓴다. screenFlush 가 g_front 와 비교해 다른 칸만 그린다.
 * 셀은 글자 + 256색 번호(글자색/배경색) + 속성이다. 역할(ROLE_*)은 넣는 순간 지금 테마의 색으로 바뀐다:
 * 테마를 바꾸면 다음 프레임을 다시 그리면 된다 (ntui 는 프레임마다 전부 다시 그린다).
 *
 * 두 가지 출력:
 *   - 화면 모드 (dev:/console, nx64_display_open 성공): 프레임버퍼에 직접. 글자 칸 = 커널 콘솔 글꼴의 칸.
 *     글자는 글꼴의 알파로 섞고, 상자 그리기(U+2500-257F)/블록(U+2580-259F)은 선/면으로 직접 그린다.
 *   - 터미널 모드: ANSI 시퀀스 (커서 이동 + SGR + ASCII). 출력은 g_out 에 모아 write 로 한꺼번에 보낸다
 *     (시리얼 115200bps 에서 차이가 크다). Nyxis 콘솔은 ASCII 만 그리므로 상자 글자는 + - | 로 바꾼다.
 *     줄 끝 칸을 쓰면 다음 글자를 쓸 때 줄이 바뀌므로 끝 칸을 쓴 뒤에는 커서 위치를 모른다고 본다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <nx64.h>

#include "ntui.h"

#define OUT_MAX 4096

struct cell {
    unsigned short ch;
    unsigned char fg, bg;
    unsigned char attr;
};

int screenCols;
int screenRows;
int screenColors = 256;                 /* 16 이면 테마의 16색 값을 쓴다 */

static struct cell *g_back;             /* 다음 프레임 */
static struct cell *g_front;            /* 화면에 지금 있는 것 */
static char g_out[OUT_MAX];
static size_t g_outLen;

static struct nx64_display g_disp;
static int g_dispOpen;
static int g_marginDirty;               /* 화면 모드: 칸 격자 밖의 가장자리를 다시 칠한다 */
static unsigned g_pal[256];             /* 256색 -> 화면 픽셀 값 */

/* ---------------- 색 ---------------- */

static unsigned paletteRgb(int i)
{
    static const unsigned base16[16] = {
        0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE5E5E5,
        0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF
    };
    static const unsigned char cube[6] = { 0, 95, 135, 175, 215, 255 };

    if (i < 16)
        return base16[i];
    if (i < 232) {
        i -= 16;
        return ((unsigned)cube[i / 36] << 16) | ((unsigned)cube[(i / 6) % 6] << 8) | cube[i % 6];
    }
    i = 8 + (i - 232) * 10;
    return ((unsigned)i << 16) | ((unsigned)i << 8) | (unsigned)i;
}

/* 256색 -> 가장 가까운 16색 (터미널 모드의 16색) */
static int to16(int c)
{
    unsigned rgb;
    int r, g, b;
    int best = 0;
    long bestD = -1;
    int i;

    if (c < 16)
        return c;
    rgb = paletteRgb(c);
    r = (int)(rgb >> 16) & 0xFF;
    g = (int)(rgb >> 8) & 0xFF;
    b = (int)rgb & 0xFF;
    for (i = 0; i < 16; i++) {
        unsigned p = paletteRgb(i);
        long dr = (long)((p >> 16) & 0xFF) - r;
        long dg = (long)((p >> 8) & 0xFF) - g;
        long db = (long)(p & 0xFF) - b;
        long d = dr * dr + dg * dg + db * db;

        if (bestD < 0 || d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

/* 역할의 지금 색 (256색 번호; 16색 화면/테마면 0-15) */
void screenStyle(enum role r, int *fg, int *bg, unsigned *attr)
{
    const struct style *s = &currentTheme->role[r];

    if (screenColors >= 256 && !currentTheme->only16) {
        *fg = s->fg;
        *bg = s->bg;
    } else {
        *fg = s->fg16;
        *bg = s->bg16;
    }
    *attr = s->attr;
}

/* ---------------- 출력 버퍼 (터미널 모드) ---------------- */

static void outFlush(void)
{
    size_t off = 0;

    while (off < g_outLen) {
        ssize_t n = write(ttyFd(), g_out + off, g_outLen - off);

        if (n <= 0)
            break;                      /* 터미널이 사라졌다: 버린다 */
        off += (size_t)n;
    }
    g_outLen = 0;
}

static void outStr(const char *s, size_t n)
{
    if (g_outLen + n > sizeof(g_out))
        outFlush();
    if (n > sizeof(g_out))
        n = sizeof(g_out);
    memcpy(g_out + g_outLen, s, n);
    g_outLen += n;
}

static void outCstr(const char *s)
{
    outStr(s, strlen(s));
}

/* ---------------- 시작 / 끝 ---------------- */

int screenDisplayMode(void)
{
    return g_dispOpen;
}

struct nx64_display *screenDisplay(void)
{
    return g_dispOpen ? &g_disp : NULL;
}

static int allocCells(void)
{
    size_t n = (size_t)screenCols * (size_t)screenRows;

    free(g_back);
    free(g_front);
    g_back = (struct cell *)calloc(n, sizeof(struct cell));
    g_front = (struct cell *)calloc(n, sizeof(struct cell));
    if (g_back == NULL || g_front == NULL) {
        free(g_back);
        free(g_front);
        g_back = g_front = NULL;
        return -1;
    }
    return 0;
}

/*
 * 화면을 연다: dev:/console 을 가져오면 화면 모드, 아니면 터미널 모드 (지금 터미널을 raw 로).
 * 설정 display (app:/ntui.run/display): "terminal" 이면 화면을 가져오지 않는다.
 */
int screenInit(void)
{
    char mode[16];
    int i;

    if (nx64_setting_get("ntui", "display", mode, sizeof(mode)) < 0 || strcmp(mode, "terminal") != 0) {
        if (nx64_display_open(&g_disp) == 0) {
            g_dispOpen = 1;
            screenCols = g_disp.width / g_disp.cell_w;
            screenRows = g_disp.height / g_disp.cell_h;
            screenColors = 256;
            for (i = 0; i < 256; i++)
                g_pal[i] = nx64_display_rgb(&g_disp, paletteRgb(i));
        }
    }
    if (inputOpen() < 0)
        return -1;
    if (!g_dispOpen) {
        struct winsize ws;

        screenCols = 80;
        screenRows = 24;
        if (ioctl(ttyFd(), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
            screenCols = ws.ws_col;
            screenRows = ws.ws_row;
        }
    }
    if (screenCols < 20 || screenRows < 6 || allocCells() < 0)
        return -1;

    if (!g_dispOpen)
        outCstr("\033[?25l");           /* 커서 숨기기 */
    screenClear(ROLE_DESKTOP);
    screenInvalidate();
    return 0;
}

/* 화면을 돌려준다 (화면 모드: 커널 콘솔로, 터미널 모드: 색/커서를 되돌리고 지운다) */
void screenEnd(void)
{
    if (g_dispOpen) {
        nx64_display_close(&g_disp);
        g_dispOpen = 0;
    } else {
        outCstr("\033[0m\033[2J\033[H\033[?25h");
        outFlush();
    }
    free(g_back);
    free(g_front);
    g_back = g_front = NULL;
}

/* ---------------- 그리기 (g_back 에만) ---------------- */

void screenPutColor(int x, int y, int ch, int fg, int bg, unsigned attr)
{
    struct cell *c;

    if (g_back == NULL || x < 0 || y < 0 || x >= screenCols || y >= screenRows)
        return;
    if (ch < 0x20 || ch == 0x7F || ch > 0xFFFF)
        ch = '?';
    if (attr & ATTR_REVERSE) {
        int t = fg;

        fg = bg;
        bg = t;
    }
    if ((attr & ATTR_BOLD) && fg < 8 && screenColors >= 16)
        fg += 8;                        /* 굵게 = 밝은 색 (커널 콘솔과 같다) */
    if (screenColors < 256) {
        fg = to16(fg);
        bg = to16(bg);
    }
    c = &g_back[y * screenCols + x];
    c->ch = (unsigned short)ch;
    c->fg = (unsigned char)fg;
    c->bg = (unsigned char)bg;
    c->attr = (unsigned char)(attr & (ATTR_BOLD | ATTR_UNDERLINE));
}

void screenPut(int x, int y, int ch, enum role r)
{
    int fg;
    int bg;
    unsigned attr;

    screenStyle(r, &fg, &bg, &attr);
    screenPutColor(x, y, ch, fg, bg, attr);
}

/* 한 줄 문자열. 화면 밖은 잘린다. 줄바꿈은 해석하지 않는다. 쓴 뒤의 x 를 돌려준다 */
int screenText(int x, int y, const char *s, enum role r)
{
    while (*s != '\0') {
        screenPut(x, y, (unsigned char)*s, r);
        s++;
        x++;
    }
    return x;
}

void screenFill(int x, int y, int w, int h, int ch, enum role r)
{
    int i;
    int j;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++)
            screenPut(x + i, y + j, ch, r);
    }
}

void screenClear(enum role r)
{
    screenFill(0, 0, screenCols, screenRows, ' ', r);
}

/* 테두리. 안쪽은 건드리지 않는다. w, h 는 테두리 포함 크기 */
void screenBox(int x, int y, int w, int h, enum role r)
{
    int i;

    if (w < 2 || h < 2)
        return;
    for (i = 1; i < w - 1; i++) {
        screenPut(x + i, y, CH_HLINE, r);
        screenPut(x + i, y + h - 1, CH_HLINE, r);
    }
    for (i = 1; i < h - 1; i++) {
        screenPut(x, y + i, CH_VLINE, r);
        screenPut(x + w - 1, y + i, CH_VLINE, r);
    }
    screenPut(x, y, CH_TL, r);
    screenPut(x + w - 1, y, CH_TR, r);
    screenPut(x, y + h - 1, CH_BL, r);
    screenPut(x + w - 1, y + h - 1, CH_BR, r);
}

/* ---------------- 상자 그리기 글자 ---------------- */

#define LN_U 1
#define LN_D 2
#define LN_L 4
#define LN_R 8
#define LN_DOUBLE 16

/* 상자 그리기 글자의 선 방향 (모르면 0) */
static int lineMask(int ch)
{
    switch (ch) {
    case 0x2500: case 0x2501: case 0x2504: case 0x2505: case 0x2508: case 0x2509: case 0x254C: case 0x254D:
        return LN_L | LN_R;
    case 0x2502: case 0x2503: case 0x2506: case 0x2507: case 0x250A: case 0x250B: case 0x254E: case 0x254F:
        return LN_U | LN_D;
    case 0x250C: case 0x250D: case 0x250E: case 0x250F: case 0x256D: return LN_R | LN_D;
    case 0x2510: case 0x2511: case 0x2512: case 0x2513: case 0x256E: return LN_L | LN_D;
    case 0x2514: case 0x2515: case 0x2516: case 0x2517: case 0x2570: return LN_U | LN_R;
    case 0x2518: case 0x2519: case 0x251A: case 0x251B: case 0x256F: return LN_U | LN_L;
    case 0x251C: case 0x251D: case 0x2520: case 0x2523: return LN_U | LN_D | LN_R;
    case 0x2524: case 0x2525: case 0x2528: case 0x252B: return LN_U | LN_D | LN_L;
    case 0x252C: case 0x252F: case 0x2530: case 0x2533: return LN_L | LN_R | LN_D;
    case 0x2534: case 0x2537: case 0x2538: case 0x253B: return LN_L | LN_R | LN_U;
    case 0x253C: case 0x253F: case 0x2542: case 0x254B: return LN_U | LN_D | LN_L | LN_R;
    case 0x2574: return LN_L;
    case 0x2575: return LN_U;
    case 0x2576: return LN_R;
    case 0x2577: return LN_D;
    case 0x2550: return LN_DOUBLE | LN_L | LN_R;
    case 0x2551: return LN_DOUBLE | LN_U | LN_D;
    case 0x2552: case 0x2553: case 0x2554: return LN_DOUBLE | LN_R | LN_D;
    case 0x2555: case 0x2556: case 0x2557: return LN_DOUBLE | LN_L | LN_D;
    case 0x2558: case 0x2559: case 0x255A: return LN_DOUBLE | LN_U | LN_R;
    case 0x255B: case 0x255C: case 0x255D: return LN_DOUBLE | LN_U | LN_L;
    case 0x255E: case 0x255F: case 0x2560: return LN_DOUBLE | LN_U | LN_D | LN_R;
    case 0x2561: case 0x2562: case 0x2563: return LN_DOUBLE | LN_U | LN_D | LN_L;
    case 0x2564: case 0x2565: case 0x2566: return LN_DOUBLE | LN_L | LN_R | LN_D;
    case 0x2567: case 0x2568: case 0x2569: return LN_DOUBLE | LN_L | LN_R | LN_U;
    case 0x256A: case 0x256B: case 0x256C: return LN_DOUBLE | LN_U | LN_D | LN_L | LN_R;
    default: return 0;
    }
}

/* 터미널 모드의 ASCII 대체 */
static char asciiOf(int ch)
{
    int m;

    if (ch >= 0x20 && ch < 0x7F)
        return (char)ch;
    m = lineMask(ch) & (LN_U | LN_D | LN_L | LN_R);
    if (m == (LN_L | LN_R) || m == LN_L || m == LN_R)
        return '-';
    if (m == (LN_U | LN_D) || m == LN_U || m == LN_D)
        return '|';
    if (m)
        return '+';
    if (ch >= 0x2580 && ch <= 0x259F)
        return ch == 0x2591 ? ':' : '#';
    if (ch == 0x00B7 || ch == 0x2022)
        return '*';
    return '?';
}

/* ---------------- 화면 모드: 셀 그리기 ---------------- */

static unsigned blend(unsigned fg, unsigned bg, unsigned a)
{
    unsigned r = (((fg >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * (255U - a)) / 255U;
    unsigned g = (((fg >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * (255U - a)) / 255U;
    unsigned b = ((fg & 0xFF) * a + (bg & 0xFF) * (255U - a)) / 255U;

    return (r << 16) | (g << 8) | b;
}

static void fillPx(int x0, int y0, int w, int h, unsigned px)
{
    int x;
    int y;

    for (y = y0; y < y0 + h; y++) {
        unsigned *line = g_disp.fb + (size_t)y * (size_t)g_disp.stride;

        for (x = x0; x < x0 + w; x++)
            line[x] = px;
    }
}

static void drawCell(int cx, int cy, const struct cell *c)
{
    int cw = g_disp.cell_w;
    int chh = g_disp.cell_h;
    int px = cx * cw;
    int py = cy * chh;
    unsigned fg = g_pal[c->fg];
    unsigned bg = g_pal[c->bg];
    int ch = c->ch;
    int m;
    int x;
    int y;

    fillPx(px, py, cw, chh, bg);
    m = lineMask(ch);
    if (m) {
        int mx = px + cw / 2;
        int my = py + chh / 2;
        int off = (m & LN_DOUBLE) ? 1 : 0;
        int k;

        for (k = -off; k <= off; k += (off ? 2 : 1)) {
            if (m & LN_L)
                fillPx(px, my + k, cw / 2 + 1, 1, fg);
            if (m & LN_R)
                fillPx(mx, my + k, cw - cw / 2, 1, fg);
            if (m & LN_U)
                fillPx(mx + k, py, 1, chh / 2 + 1, fg);
            if (m & LN_D)
                fillPx(mx + k, my, 1, chh - chh / 2, fg);
        }
    } else if (ch >= 0x2580 && ch <= 0x2593) {
        switch (ch) {
        case 0x2588: fillPx(px, py, cw, chh, fg); break;
        case 0x2580: fillPx(px, py, cw, chh / 2, fg); break;
        case 0x2584: fillPx(px, py + chh / 2, cw, chh - chh / 2, fg); break;
        case 0x258C: fillPx(px, py, cw / 2, chh, fg); break;
        case 0x2590: fillPx(px + cw / 2, py, cw - cw / 2, chh, fg); break;
        case 0x2591: case 0x2592: case 0x2593: {
            unsigned a = ch == 0x2591 ? 64U : ch == 0x2592 ? 128U : 192U;

            fillPx(px, py, cw, chh, blend(fg, bg, a));
            break;
        }
        default:
            fillPx(px, py, cw, chh, blend(fg, bg, 128U));
            break;
        }
    } else {
        int idx = (ch >= 0x20 && ch < 0x7F ? ch : asciiOf(ch)) - g_disp.font_first;

        if (idx >= 0 && idx < g_disp.font_count && ch != ' ') {
            const unsigned char *alpha = g_disp.font + (size_t)idx * (size_t)(cw * chh);

            for (y = 0; y < chh; y++) {
                unsigned *line = g_disp.fb + (size_t)(py + y) * (size_t)g_disp.stride + px;

                for (x = 0; x < cw; x++) {
                    unsigned a = alpha[y * cw + x];

                    if (a == 255U)
                        line[x] = fg;
                    else if (a != 0U)
                        line[x] = blend(fg, bg, a);
                }
            }
        }
    }
    if (c->attr & ATTR_UNDERLINE)
        fillPx(px, py + chh - 1, cw, 1, fg);
}

/* 칸 격자 밖(오른쪽/아래 가장자리)을 바탕화면 색으로 */
static void drawMargins(void)
{
    int fg;
    int bg;
    unsigned attr;
    int gw = screenCols * g_disp.cell_w;
    int gh = screenRows * g_disp.cell_h;
    unsigned px;

    screenStyle(ROLE_DESKTOP, &fg, &bg, &attr);
    px = g_pal[bg];
    if (gw < g_disp.width)
        fillPx(gw, 0, g_disp.width - gw, g_disp.height, px);
    if (gh < g_disp.height)
        fillPx(0, gh, gw, g_disp.height - gh, px);
}

/* ---------------- 내보내기 ---------------- */

/* 다음 screenFlush 가 모든 칸을 다시 그리게 한다 (테마 변경, 화면이 다른 출력으로 망가졌을 때) */
void screenInvalidate(void)
{
    size_t i;
    size_t n = (size_t)screenCols * (size_t)screenRows;

    if (g_front == NULL)
        return;
    for (i = 0; i < n; i++) {
        g_front[i].ch = 0;              /* 그릴 수 있는 글자가 아니므로 늘 다르다 */
        g_front[i].attr = 0xFF;
    }
    g_marginDirty = 1;
}

static void sgr(const struct cell *c)
{
    char seq[48];

    if (screenColors >= 256) {
        snprintf(seq, sizeof(seq), "\033[0%s;38;5;%u;48;5;%um", (c->attr & ATTR_BOLD) ? ";1" : "",
                 (unsigned)c->fg, (unsigned)c->bg);
    } else {
        unsigned fg = c->fg < 8 ? 30U + c->fg : 90U + (c->fg - 8U);
        unsigned bg = c->bg < 8 ? 40U + c->bg : 100U + (c->bg - 8U);

        snprintf(seq, sizeof(seq), "\033[0%s;%u;%um", (c->attr & ATTR_BOLD) ? ";1" : "", fg, bg);
    }
    outCstr(seq);
}

static int sameCell(const struct cell *a, const struct cell *b)
{
    return a->ch == b->ch && a->fg == b->fg && a->bg == b->bg && a->attr == b->attr;
}

/* g_back 과 g_front 의 다른 칸만 그리고 g_front 를 맞춘다 */
void screenFlush(void)
{
    char seq[48];
    int x;
    int y;
    int curX = -1;                      /* 터미널 커서 위치 (-1 = 모름) */
    int curY = -1;
    struct cell curStyle;
    int haveStyle = 0;

    if (g_back == NULL)
        return;
    if (g_dispOpen && g_marginDirty) {
        drawMargins();
        g_marginDirty = 0;
    }
    for (y = 0; y < screenRows; y++) {
        for (x = 0; x < screenCols; x++) {
            struct cell *b = &g_back[y * screenCols + x];
            struct cell *f = &g_front[y * screenCols + x];

            if (sameCell(b, f))
                continue;
            *f = *b;
            if (g_dispOpen) {
                drawCell(x, y, b);
                continue;
            }
            if (x != curX || y != curY) {
                snprintf(seq, sizeof(seq), "\033[%d;%dH", y + 1, x + 1);
                outCstr(seq);
            }
            if (!haveStyle || curStyle.fg != b->fg || curStyle.bg != b->bg || curStyle.attr != b->attr) {
                sgr(b);
                curStyle = *b;
                haveStyle = 1;
            }
            seq[0] = asciiOf(b->ch);
            outStr(seq, 1);
            curX = x + 1;
            curY = y;
            if (curX >= screenCols)
                curX = -1;              /* 줄 끝: 다음 글자에서 줄이 바뀌므로 위치를 모른다고 본다 */
        }
    }
    if (!g_dispOpen)
        outFlush();
}

/* ---------------- 스크린샷 (PrintScreen) ---------------- */

/*
 * 화면 모드에서 지금 화면을 PPM(P6) 으로 저장한다: face:/screenshot-YYYYMMDD-HHMMSS.ppm (홈에 못 쓰면 tmp:/).
 * 저장한 경로를 path 에 넣는다. 성공 0, 실패 -1 (터미널 모드도 -1)
 */
int screenShot(char *path, size_t cap)
{
    static const char *const dirs[] = { "face:/", "tmp:/" };
    unsigned char *row;
    char name[64];
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    size_t d;
    int x;
    int y;

    if (!g_dispOpen)
        return -1;
    if (tm != NULL)
        strftime(name, sizeof(name), "screenshot-%Y%m%d-%H%M%S.ppm", tm);
    else
        strcpy(name, "screenshot.ppm");
    row = (unsigned char *)malloc((size_t)g_disp.width * 3U);
    if (row == NULL)
        return -1;
    for (d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        FILE *f;
        int ok = 1;

        snprintf(path, cap, "%s%s", dirs[d], name);
        f = fopen(path, "wb");
        if (f == NULL)
            continue;
        fprintf(f, "P6\n%d %d\n255\n", g_disp.width, g_disp.height);
        for (y = 0; y < g_disp.height && ok; y++) {
            const unsigned *line = g_disp.fb + (size_t)y * (size_t)g_disp.stride;

            for (x = 0; x < g_disp.width; x++) {
                unsigned px = line[x];
                unsigned r = g_disp.bgr ? (px & 0xFFU) : ((px >> 16) & 0xFFU);
                unsigned b = g_disp.bgr ? ((px >> 16) & 0xFFU) : (px & 0xFFU);

                row[x * 3] = (unsigned char)r;
                row[x * 3 + 1] = (unsigned char)((px >> 8) & 0xFFU);
                row[x * 3 + 2] = (unsigned char)b;
            }
            ok = fwrite(row, 3, (size_t)g_disp.width, f) == (size_t)g_disp.width;
        }
        if (fclose(f) != 0)
            ok = 0;
        if (ok) {
            free(row);
            return 0;
        }
        (void)remove(path);
    }
    free(row);
    return -1;
}
