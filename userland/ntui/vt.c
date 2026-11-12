/*
 * vt.c - 터미널 에뮬레이터 (VT100 / xterm 의 부분집합)
 *
 * 프로그램 출력(바이트)을 해석해 글자 칸 격자(cols x rows)를 만든다. 화면에 그리는 것은 nterminal.c 가 한다.
 *
 * 해석하는 것:
 *   제어 문자   BEL(무시) BS HT LF VT FF CR (LF 는 줄바꿈 + 줄 처음: Nyxis 콘솔과 같다), CAN/SUB(시퀀스 취소)
 *   ESC 7 / 8   커서 저장/복원      ESC c 초기화       ESC D 아래로(IND)  ESC E 다음 줄(NEL)  ESC M 위로(RI)
 *   ESC ( 0 / ESC ( B   DEC 선 그리기 문자 집합 켜기/끄기 (G0)
 *   CSI @ A B C D E F G H J K L M P S T X a b d e f m n r s u ` 와 h/l 모드:
 *     4 끼워 넣기, ?1 방향키 응용 모드, ?6 원점 모드, ?7 자동 줄바꿈, ?25 커서, ?47 ?1047 ?1049 보조 화면
 *   SGR 0 1 4 7 22 24 27 30-37 39 40-47 49 90-97 100-107, 38/48 ;5;n (256색) 과 ;2;r;g;b (가까운 256색으로)
 *   DSR 5n / 6n (커서 위치 보고), DA (CSI c) -> 응답 함수로
 *   OSC / DCS / PM / APC 문자열은 끝(BEL 또는 ESC \)까지 버린다
 *   UTF-8: U+2500-259F(상자/블록)은 그대로, 그 밖의 ASCII 아닌 글자는 '?' (글꼴이 ASCII 뿐이다)
 * 줄 끝에서 쓴 뒤의 줄바꿈은 다음 글자까지 미룬다 (xterm 과 같다). 줄 처음의 BS 는 윗줄 끝으로 간다
 * (커널 줄 편집기가 여러 줄에 걸친 입력을 BS 로 되돌아가며 고친다).
 *
 * 스크롤백: 주 화면에서 맨 위로 밀려난 줄을 VT_SCROLLBACK 줄까지 기억한다 (보조 화면에서는 없다).
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ntui.h"

#define VT_MAX_COLS   256
#define VT_MAX_ROWS   200
#define VT_SCROLLBACK 400
#define VT_PARAMS     16

enum { S_GROUND, S_ESC, S_CSI, S_STR, S_STR_ESC, S_CHARSET };

struct vt {
    int cols, rows;
    struct vtcell *mainGrid;
    struct vtcell *altGrid;
    struct vtcell *grid;                /* 지금 화면 (mainGrid 또는 altGrid) */
    int alt;

    int cx, cy;
    int wrapPending;                    /* 줄 끝 칸에 썼다: 다음 글자에서 줄을 바꾼다 */
    int top, bot;                       /* 스크롤 영역 (포함) */
    int savedX, savedY;
    unsigned char savedFg, savedBg, savedAttr;
    int altSavedX, altSavedY;           /* ?1049 가 저장한 커서 */

    unsigned char fg, bg, attr;         /* 지금 SGR */
    int cursorVisible;
    int autowrap;
    int appCursor;
    int insertMode;
    int originMode;
    int lineDrawing;                    /* G0 = DEC 선 그리기 */
    int lastCh;                         /* REP (CSI b) */

    struct vtcell *sb;                  /* 스크롤백 (VT_SCROLLBACK x VT_MAX_COLS, 처음 쓸 때 만든다) */
    int sbHead;
    int sbCount;

    int state;
    int params[VT_PARAMS];
    int nparams;
    int priv;                           /* CSI 의 '?' 등 */
    unsigned utf;                       /* UTF-8 모으는 중인 코드 포인트 */
    int utfNeed;

    vt_reply_fn reply;
    void *ctx;
};

/* ---------------- 셀 ---------------- */

static struct vtcell blankCell(const struct vt *v)
{
    struct vtcell c;

    c.ch = ' ';
    c.fg = v->fg;
    c.bg = v->bg;
    c.attr = (unsigned char)(v->attr & (VT_ATTR_FGDEF | VT_ATTR_BGDEF | VT_ATTR_REVERSE));
    return c;
}

static struct vtcell *rowPtr(struct vt *v, int y)
{
    return v->grid + (size_t)y * (size_t)v->cols;
}

static void fillCells(struct vtcell *p, int n, struct vtcell c)
{
    int i;

    for (i = 0; i < n; i++)
        p[i] = c;
}

static void clearGrid(struct vt *v, struct vtcell *g)
{
    struct vtcell c;

    c.ch = ' ';
    c.fg = 7;
    c.bg = 0;
    c.attr = VT_ATTR_FGDEF | VT_ATTR_BGDEF;
    fillCells(g, v->cols * v->rows, c);
}

/* ---------------- 스크롤백 ---------------- */

static void sbPush(struct vt *v, const struct vtcell *line)
{
    struct vtcell *dst;
    struct vtcell c;
    int slot;

    if (v->sb == NULL) {
        v->sb = (struct vtcell *)malloc(sizeof(struct vtcell) * VT_SCROLLBACK * VT_MAX_COLS);
        if (v->sb == NULL)
            return;
    }
    if (v->sbCount < VT_SCROLLBACK) {
        slot = (v->sbHead + v->sbCount) % VT_SCROLLBACK;
        v->sbCount++;
    } else {
        slot = v->sbHead;
        v->sbHead = (v->sbHead + 1) % VT_SCROLLBACK;
    }
    dst = v->sb + (size_t)slot * VT_MAX_COLS;
    memcpy(dst, line, sizeof(struct vtcell) * (size_t)v->cols);
    c.ch = ' ';
    c.fg = 7;
    c.bg = 0;
    c.attr = VT_ATTR_FGDEF | VT_ATTR_BGDEF;
    fillCells(dst + v->cols, VT_MAX_COLS - v->cols, c);
}

static const struct vtcell *sbLine(const struct vt *v, int i)
{
    return v->sb + (size_t)((v->sbHead + i) % VT_SCROLLBACK) * VT_MAX_COLS;
}

/* ---------------- 스크롤 ---------------- */

static void scrollUp(struct vt *v, int top, int bot, int n)
{
    int h = bot - top + 1;
    int i;

    if (n <= 0)
        return;
    if (n > h)
        n = h;
    if (!v->alt && top == 0) {
        for (i = 0; i < n; i++)
            sbPush(v, rowPtr(v, i));
    }
    memmove(rowPtr(v, top), rowPtr(v, top + n), sizeof(struct vtcell) * (size_t)(h - n) * (size_t)v->cols);
    fillCells(rowPtr(v, bot - n + 1), n * v->cols, blankCell(v));
}

static void scrollDown(struct vt *v, int top, int bot, int n)
{
    int h = bot - top + 1;

    if (n <= 0)
        return;
    if (n > h)
        n = h;
    memmove(rowPtr(v, top + n), rowPtr(v, top), sizeof(struct vtcell) * (size_t)(h - n) * (size_t)v->cols);
    fillCells(rowPtr(v, top), n * v->cols, blankCell(v));
}

/* 아래로 한 줄 (스크롤 영역의 맨 아래면 영역을 올린다) */
static void lineFeed(struct vt *v)
{
    v->wrapPending = 0;
    if (v->cy == v->bot)
        scrollUp(v, v->top, v->bot, 1);
    else if (v->cy < v->rows - 1)
        v->cy++;
}

static void reverseIndex(struct vt *v)
{
    v->wrapPending = 0;
    if (v->cy == v->top)
        scrollDown(v, v->top, v->bot, 1);
    else if (v->cy > 0)
        v->cy--;
}

/* ---------------- 만들기 / 크기 ---------------- */

static void resetState(struct vt *v)
{
    v->cx = v->cy = 0;
    v->wrapPending = 0;
    v->top = 0;
    v->bot = v->rows - 1;
    v->fg = 7;
    v->bg = 0;
    v->attr = VT_ATTR_FGDEF | VT_ATTR_BGDEF;
    v->savedX = v->savedY = 0;
    v->savedFg = v->fg;
    v->savedBg = v->bg;
    v->savedAttr = v->attr;
    v->cursorVisible = 1;
    v->autowrap = 1;
    v->appCursor = 0;
    v->insertMode = 0;
    v->originMode = 0;
    v->lineDrawing = 0;
    v->lastCh = ' ';
    v->state = S_GROUND;
    v->utfNeed = 0;
}

static void clampSize(int *cols, int *rows)
{
    if (*cols < 2)
        *cols = 2;
    if (*cols > VT_MAX_COLS)
        *cols = VT_MAX_COLS;
    if (*rows < 1)
        *rows = 1;
    if (*rows > VT_MAX_ROWS)
        *rows = VT_MAX_ROWS;
}

struct vt *vtNew(int cols, int rows, vt_reply_fn reply, void *ctx)
{
    struct vt *v = (struct vt *)calloc(1, sizeof(*v));

    if (v == NULL)
        return NULL;
    clampSize(&cols, &rows);
    v->cols = cols;
    v->rows = rows;
    v->mainGrid = (struct vtcell *)malloc(sizeof(struct vtcell) * (size_t)cols * (size_t)rows);
    v->altGrid = (struct vtcell *)malloc(sizeof(struct vtcell) * (size_t)cols * (size_t)rows);
    if (v->mainGrid == NULL || v->altGrid == NULL) {
        vtFree(v);
        return NULL;
    }
    v->grid = v->mainGrid;
    v->reply = reply;
    v->ctx = ctx;
    resetState(v);
    clearGrid(v, v->mainGrid);
    clearGrid(v, v->altGrid);
    return v;
}

void vtFree(struct vt *v)
{
    if (v == NULL)
        return;
    free(v->mainGrid);
    free(v->altGrid);
    free(v->sb);
    free(v);
}

/* 옛 격자 g(oc x orows)의 줄 [from, from+n) 을 새 격자로 옮긴다 */
static void copyRows(struct vtcell *dst, int nc, const struct vtcell *src, int oc, int from, int n)
{
    int y;
    int w = oc < nc ? oc : nc;

    for (y = 0; y < n; y++)
        memcpy(dst + (size_t)y * (size_t)nc, src + (size_t)(from + y) * (size_t)oc, sizeof(struct vtcell) * (size_t)w);
}

void vtResize(struct vt *v, int cols, int rows)
{
    struct vtcell *nm;
    struct vtcell *na;
    int oc = v->cols;
    int orows = v->rows;
    int shift;
    int keep;
    int i;

    clampSize(&cols, &rows);
    if (cols == v->cols && rows == v->rows)
        return;
    nm = (struct vtcell *)malloc(sizeof(struct vtcell) * (size_t)cols * (size_t)rows);
    na = (struct vtcell *)malloc(sizeof(struct vtcell) * (size_t)cols * (size_t)rows);
    if (nm == NULL || na == NULL) {
        free(nm);
        free(na);
        return;                         /* 메모리가 없으면 옛 크기 그대로 */
    }

    /* 커서 줄이 보이도록 위를 잘라 낸다 (주 화면이면 잘린 줄은 스크롤백으로) */
    shift = v->cy - (rows - 1);
    if (shift < 0)
        shift = 0;
    keep = orows - shift < rows ? orows - shift : rows;
    if (!v->alt) {
        for (i = 0; i < shift; i++)
            sbPush(v, v->mainGrid + (size_t)i * (size_t)oc);
    }
    v->cols = cols;                     /* clearGrid 가 새 크기를 쓴다 */
    v->rows = rows;
    clearGrid(v, nm);
    clearGrid(v, na);
    copyRows(nm, cols, v->mainGrid, oc, v->alt ? 0 : shift, v->alt ? (orows < rows ? orows : rows) : keep);
    copyRows(na, cols, v->altGrid, oc, v->alt ? shift : 0, v->alt ? keep : (orows < rows ? orows : rows));
    free(v->mainGrid);
    free(v->altGrid);
    v->mainGrid = nm;
    v->altGrid = na;
    v->grid = v->alt ? na : nm;

    v->cy -= shift;
    if (v->cx >= cols)
        v->cx = cols - 1;
    if (v->cy >= rows)
        v->cy = rows - 1;
    v->wrapPending = 0;
    v->top = 0;
    v->bot = rows - 1;
    if (v->savedX >= cols)
        v->savedX = cols - 1;
    if (v->savedY >= rows)
        v->savedY = rows - 1;
}

int vtCols(const struct vt *v)
{
    return v->cols;
}

int vtRows(const struct vt *v)
{
    return v->rows;
}

int vtScrollback(const struct vt *v)
{
    return v->alt ? 0 : v->sbCount;
}

const struct vtcell *vtLine(const struct vt *v, int y, int scroll)
{
    int idx;

    if (v->alt || scroll <= 0)
        return v->grid + (size_t)y * (size_t)v->cols;
    if (scroll > v->sbCount)
        scroll = v->sbCount;
    idx = v->sbCount - scroll + y;
    if (idx < v->sbCount)
        return sbLine(v, idx);
    return v->grid + (size_t)(idx - v->sbCount) * (size_t)v->cols;
}

int vtCursor(const struct vt *v, int *x, int *y)
{
    *x = v->cx;
    *y = v->cy;
    return v->cursorVisible;
}

int vtAppCursorKeys(const struct vt *v)
{
    return v->appCursor;
}

/* ---------------- 글자 ---------------- */

/* DEC 선 그리기 집합 (ESC ( 0): 0x60-0x7E -> 유니코드 */
static int decGraphics(int c)
{
    switch (c) {
    case 'j': return 0x2518;
    case 'k': return 0x2510;
    case 'l': return 0x250C;
    case 'm': return 0x2514;
    case 'n': return 0x253C;
    case 'q': return 0x2500;
    case 't': return 0x251C;
    case 'u': return 0x2524;
    case 'v': return 0x2534;
    case 'w': return 0x252C;
    case 'x': return 0x2502;
    case 'a': return 0x2592;
    case '`': return '*';
    case '~': return '*';
    case 'o': case 'p': case 'r': case 's': return 0x2500;
    default: return c;
    }
}

static void putGlyph(struct vt *v, int ch)
{
    struct vtcell *row;
    struct vtcell c;

    if (v->lineDrawing && ch >= 0x60 && ch <= 0x7E)
        ch = decGraphics(ch);
    if (ch > 0x7E && !(ch >= 0x2500 && ch <= 0x259F))
        ch = '?';
    if (v->wrapPending && v->autowrap) {
        v->cx = 0;
        lineFeed(v);
    }
    v->wrapPending = 0;
    row = rowPtr(v, v->cy);
    if (v->insertMode && v->cx < v->cols - 1)
        memmove(row + v->cx + 1, row + v->cx, sizeof(struct vtcell) * (size_t)(v->cols - v->cx - 1));
    c.ch = (unsigned short)ch;
    c.fg = v->fg;
    c.bg = v->bg;
    c.attr = v->attr;
    row[v->cx] = c;
    v->lastCh = ch;
    if (v->cx == v->cols - 1) {
        if (v->autowrap)
            v->wrapPending = 1;
    } else {
        v->cx++;
    }
}

/* ---------------- CSI ---------------- */

static int param(const struct vt *v, int i, int def)
{
    if (i >= v->nparams || v->params[i] <= 0)
        return def;
    return v->params[i];
}

static void clampCursor(struct vt *v)
{
    int minY = v->originMode ? v->top : 0;
    int maxY = v->originMode ? v->bot : v->rows - 1;

    if (v->cx < 0)
        v->cx = 0;
    if (v->cx >= v->cols)
        v->cx = v->cols - 1;
    if (v->cy < minY)
        v->cy = minY;
    if (v->cy > maxY)
        v->cy = maxY;
    v->wrapPending = 0;
}

static void eraseInDisplay(struct vt *v, int mode)
{
    struct vtcell b = blankCell(v);

    if (mode == 0) {
        fillCells(rowPtr(v, v->cy) + v->cx, v->cols - v->cx, b);
        if (v->cy + 1 < v->rows)
            fillCells(rowPtr(v, v->cy + 1), (v->rows - v->cy - 1) * v->cols, b);
    } else if (mode == 1) {
        fillCells(rowPtr(v, 0), v->cy * v->cols, b);
        fillCells(rowPtr(v, v->cy), v->cx + 1, b);
    } else {
        fillCells(rowPtr(v, 0), v->rows * v->cols, b);
    }
}

static void eraseInLine(struct vt *v, int mode)
{
    struct vtcell b = blankCell(v);
    struct vtcell *row = rowPtr(v, v->cy);

    if (mode == 0)
        fillCells(row + v->cx, v->cols - v->cx, b);
    else if (mode == 1)
        fillCells(row, v->cx + 1, b);
    else
        fillCells(row, v->cols, b);
}

/* 24비트 색 -> 가까운 256색 (6x6x6 큐브 또는 회색) */
static int rgbTo256(int r, int g, int b)
{
    int ri = r < 48 ? 0 : r < 115 ? 1 : (r - 35) / 40;
    int gi = g < 48 ? 0 : g < 115 ? 1 : (g - 35) / 40;
    int bi = b < 48 ? 0 : b < 115 ? 1 : (b - 35) / 40;

    if (r == g && g == b) {
        if (r < 8)
            return 16;
        if (r > 248)
            return 231;
        return 232 + (r - 8) / 10;
    }
    return 16 + 36 * ri + 6 * gi + bi;
}

static void sgr(struct vt *v)
{
    int i;

    if (v->nparams == 0) {
        v->fg = 7;
        v->bg = 0;
        v->attr = VT_ATTR_FGDEF | VT_ATTR_BGDEF;
        return;
    }
    for (i = 0; i < v->nparams; i++) {
        int p = v->params[i] < 0 ? 0 : v->params[i];

        if (p == 0) {
            v->fg = 7;
            v->bg = 0;
            v->attr = VT_ATTR_FGDEF | VT_ATTR_BGDEF;
        } else if (p == 1) {
            v->attr |= VT_ATTR_BOLD;
        } else if (p == 22) {
            v->attr &= (unsigned char)~VT_ATTR_BOLD;
        } else if (p == 4) {
            v->attr |= VT_ATTR_UNDERLINE;
        } else if (p == 24) {
            v->attr &= (unsigned char)~VT_ATTR_UNDERLINE;
        } else if (p == 7) {
            v->attr |= VT_ATTR_REVERSE;
        } else if (p == 27) {
            v->attr &= (unsigned char)~VT_ATTR_REVERSE;
        } else if (p >= 30 && p <= 37) {
            v->fg = (unsigned char)(p - 30);
            v->attr &= (unsigned char)~VT_ATTR_FGDEF;
        } else if (p >= 90 && p <= 97) {
            v->fg = (unsigned char)(p - 90 + 8);
            v->attr &= (unsigned char)~VT_ATTR_FGDEF;
        } else if (p == 39) {
            v->attr |= VT_ATTR_FGDEF;
        } else if (p >= 40 && p <= 47) {
            v->bg = (unsigned char)(p - 40);
            v->attr &= (unsigned char)~VT_ATTR_BGDEF;
        } else if (p >= 100 && p <= 107) {
            v->bg = (unsigned char)(p - 100 + 8);
            v->attr &= (unsigned char)~VT_ATTR_BGDEF;
        } else if (p == 49) {
            v->attr |= VT_ATTR_BGDEF;
        } else if (p == 38 || p == 48) {
            int c = -1;

            if (i + 2 < v->nparams && v->params[i + 1] == 5) {
                c = v->params[i + 2] & 0xFF;
                i += 2;
            } else if (i + 4 < v->nparams && v->params[i + 1] == 2) {
                c = rgbTo256(v->params[i + 2] & 0xFF, v->params[i + 3] & 0xFF, v->params[i + 4] & 0xFF);
                i += 4;
            } else {
                break;                  /* 모르는 형식: 나머지를 버린다 */
            }
            if (p == 38) {
                v->fg = (unsigned char)c;
                v->attr &= (unsigned char)~VT_ATTR_FGDEF;
            } else {
                v->bg = (unsigned char)c;
                v->attr &= (unsigned char)~VT_ATTR_BGDEF;
            }
        }
        /* 그 밖의 속성(기울임, 깜빡임 ...)은 무시 */
    }
}

static void saveCursor(struct vt *v)
{
    v->savedX = v->cx;
    v->savedY = v->cy;
    v->savedFg = v->fg;
    v->savedBg = v->bg;
    v->savedAttr = v->attr;
}

static void restoreCursor(struct vt *v)
{
    v->cx = v->savedX;
    v->cy = v->savedY;
    v->fg = v->savedFg;
    v->bg = v->savedBg;
    v->attr = v->savedAttr;
    clampCursor(v);
}

static void setAlt(struct vt *v, int on, int saveCur)
{
    if (on == v->alt)
        return;
    if (on) {
        if (saveCur) {
            v->altSavedX = v->cx;
            v->altSavedY = v->cy;
        }
        v->alt = 1;
        v->grid = v->altGrid;
        clearGrid(v, v->altGrid);
    } else {
        v->alt = 0;
        v->grid = v->mainGrid;
        if (saveCur) {
            v->cx = v->altSavedX;
            v->cy = v->altSavedY;
        }
    }
    v->top = 0;
    v->bot = v->rows - 1;
    clampCursor(v);
}

static void setMode(struct vt *v, int on)
{
    int i;

    for (i = 0; i < v->nparams; i++) {
        int p = v->params[i];

        if (v->priv == '?') {
            switch (p) {
            case 1:    v->appCursor = on; break;
            case 6:    v->originMode = on; v->cx = 0; v->cy = on ? v->top : 0; break;
            case 7:    v->autowrap = on; if (!on) v->wrapPending = 0; break;
            case 25:   v->cursorVisible = on; break;
            case 47:
            case 1047: setAlt(v, on, 0); break;
            case 1049: setAlt(v, on, 1); break;
            default: break;
            }
        } else if (v->priv == 0 && p == 4) {
            v->insertMode = on;
        }
    }
}

static void reply(struct vt *v, const char *s)
{
    if (v->reply)
        v->reply(v->ctx, s, (int)strlen(s));
}

static void csiDispatch(struct vt *v, int final)
{
    struct vtcell *row;
    int n = param(v, 0, 1);
    int i;

    if (v->priv != 0 && final != 'h' && final != 'l' && final != 'c' && final != 'n')
        return;                         /* 모르는 사적 시퀀스 */

    switch (final) {
    case 'A': {                         /* 스크롤 영역 안에서는 영역 위에서 멈춘다 */
        int lim = v->cy >= v->top ? v->top : 0;

        v->cy = v->cy - n < lim ? lim : v->cy - n;
        clampCursor(v);
        break;
    }
    case 'B': {
        int lim = v->cy <= v->bot ? v->bot : v->rows - 1;

        v->cy = v->cy + n > lim ? lim : v->cy + n;
        clampCursor(v);
        break;
    }
    case 'C': case 'a': v->cx += n; clampCursor(v); break;
    case 'D': v->cx -= n; clampCursor(v); break;
    case 'E': v->cx = 0; v->cy += n; clampCursor(v); break;
    case 'F': v->cx = 0; v->cy -= n; clampCursor(v); break;
    case 'G': case '`': v->cx = n - 1; clampCursor(v); break;
    case 'd': v->cy = (v->originMode ? v->top : 0) + n - 1; clampCursor(v); break;
    case 'e': v->cy += n; clampCursor(v); break;
    case 'H': case 'f':
        v->cy = (v->originMode ? v->top : 0) + param(v, 0, 1) - 1;
        v->cx = param(v, 1, 1) - 1;
        clampCursor(v);
        break;
    case 'J': eraseInDisplay(v, v->nparams ? v->params[0] : 0); break;
    case 'K': eraseInLine(v, v->nparams ? v->params[0] : 0); break;
    case '@':
        row = rowPtr(v, v->cy);
        if (n > v->cols - v->cx)
            n = v->cols - v->cx;
        memmove(row + v->cx + n, row + v->cx, sizeof(struct vtcell) * (size_t)(v->cols - v->cx - n));
        fillCells(row + v->cx, n, blankCell(v));
        v->wrapPending = 0;
        break;
    case 'P':
        row = rowPtr(v, v->cy);
        if (n > v->cols - v->cx)
            n = v->cols - v->cx;
        memmove(row + v->cx, row + v->cx + n, sizeof(struct vtcell) * (size_t)(v->cols - v->cx - n));
        fillCells(row + v->cols - n, n, blankCell(v));
        v->wrapPending = 0;
        break;
    case 'X':
        if (n > v->cols - v->cx)
            n = v->cols - v->cx;
        fillCells(rowPtr(v, v->cy) + v->cx, n, blankCell(v));
        v->wrapPending = 0;
        break;
    case 'L':
        if (v->cy >= v->top && v->cy <= v->bot) {
            scrollDown(v, v->cy, v->bot, n);
            v->cx = 0;
            v->wrapPending = 0;
        }
        break;
    case 'M':
        if (v->cy >= v->top && v->cy <= v->bot) {
            /* 지우는 줄은 스크롤백으로 보내지 않는다 (scrollUp 은 top == 0 일 때 보낸다) */
            int h = v->bot - v->cy + 1;

            if (n > h)
                n = h;
            memmove(rowPtr(v, v->cy), rowPtr(v, v->cy + n), sizeof(struct vtcell) * (size_t)(h - n) * (size_t)v->cols);
            fillCells(rowPtr(v, v->bot - n + 1), n * v->cols, blankCell(v));
            v->cx = 0;
            v->wrapPending = 0;
        }
        break;
    case 'S': scrollUp(v, v->top, v->bot, n); break;
    case 'T': scrollDown(v, v->top, v->bot, n); break;
    case 'b':
        for (i = 0; i < n && i < 1000; i++)
            putGlyph(v, v->lastCh);
        break;
    case 'm': sgr(v); break;
    case 'r': {
        int t = param(v, 0, 1) - 1;
        int b = param(v, 1, v->rows) - 1;

        if (b >= v->rows)
            b = v->rows - 1;
        if (t < b) {
            v->top = t;
            v->bot = b;
            v->cx = 0;
            v->cy = v->originMode ? t : 0;
            v->wrapPending = 0;
        }
        break;
    }
    case 's': saveCursor(v); break;
    case 'u': restoreCursor(v); break;
    case 'h': setMode(v, 1); break;
    case 'l': setMode(v, 0); break;
    case 'n':
        if (v->nparams && v->params[0] == 5) {
            reply(v, "\033[0n");
        } else if (v->nparams && v->params[0] == 6) {
            char buf[32];

            snprintf(buf, sizeof(buf), "\033[%d;%dR", v->cy - (v->originMode ? v->top : 0) + 1, v->cx + 1);
            reply(v, buf);
        }
        break;
    case 'c':
        if (v->priv == 0 && param(v, 0, 0) == 0)
            reply(v, "\033[?1;2c");     /* VT100 + 고급 비디오 */
        break;
    default:
        break;
    }
}

/* ---------------- 바이트 해석 ---------------- */

static void control(struct vt *v, int c)
{
    switch (c) {
    case 0x08:                          /* BS */
        if (v->wrapPending)
            v->wrapPending = 0;         /* 줄 끝 칸 그대로 (그 칸의 글자를 고치러 간다) */
        else if (v->cx > 0)
            v->cx--;
        else if (v->cy > 0 && v->autowrap) {
            v->cy--;                    /* 윗줄 끝으로 (여러 줄에 걸친 입력 편집) */
            v->cx = v->cols - 1;
        }
        break;
    case 0x09: {                        /* HT */
        int nx = (v->cx / 8 + 1) * 8;

        v->cx = nx >= v->cols ? v->cols - 1 : nx;
        v->wrapPending = 0;
        break;
    }
    case 0x0A: case 0x0B: case 0x0C:    /* LF VT FF: 줄바꿈 + 줄 처음 */
        lineFeed(v);
        v->cx = 0;
        break;
    case 0x0D:
        v->cx = 0;
        v->wrapPending = 0;
        break;
    case 0x0E: case 0x0F:               /* SO/SI: G1/G0 (G1 은 다루지 않는다) */
        break;
    default:
        break;                          /* BEL 등 */
    }
}

static void escDispatch(struct vt *v, int c)
{
    v->state = S_GROUND;
    switch (c) {
    case '[':
        v->state = S_CSI;
        v->nparams = 0;
        v->priv = 0;
        memset(v->params, 0, sizeof(v->params));
        break;
    case ']': case 'P': case 'X': case '^': case '_':
        v->state = S_STR;
        break;
    case '(':
        v->state = S_CHARSET;
        break;
    case ')': case '*': case '+':
        v->state = S_CHARSET;           /* G1-G3: 다음 바이트를 버린다 (lineDrawing 은 G0 만) */
        v->priv = -1;
        break;
    case '7': saveCursor(v); break;
    case '8': restoreCursor(v); break;
    case 'D': lineFeed(v); break;
    case 'E': lineFeed(v); v->cx = 0; break;
    case 'M': reverseIndex(v); break;
    case 'c':
        resetState(v);
        v->alt = 0;
        v->grid = v->mainGrid;
        clearGrid(v, v->mainGrid);
        break;
    default:
        break;                          /* ESC = > 등 */
    }
}

static void feedByte(struct vt *v, unsigned char c)
{
    if (c == 0x18 || c == 0x1A) {       /* CAN / SUB: 시퀀스 취소 */
        v->state = S_GROUND;
        v->utfNeed = 0;
        return;
    }

    switch (v->state) {
    case S_ESC:
        if (c < 0x20) {
            control(v, c);
            return;
        }
        escDispatch(v, c);
        return;
    case S_CSI:
        if (c == 0x1B) {
            v->state = S_ESC;
            return;
        }
        if (c < 0x20) {
            control(v, c);              /* CSI 안의 제어 문자는 실행한다 (VT100) */
            return;
        }
        if (c >= '0' && c <= '9') {
            if (v->nparams == 0)
                v->nparams = 1;
            if (v->params[v->nparams - 1] < 10000)
                v->params[v->nparams - 1] = v->params[v->nparams - 1] * 10 + (c - '0');
            return;
        }
        if (c == ';' || c == ':') {
            if (v->nparams == 0)
                v->nparams = 1;
            if (v->nparams < VT_PARAMS)
                v->params[v->nparams++] = 0;
            return;
        }
        if (c >= 0x3C && c <= 0x3F) {   /* < = > ? */
            v->priv = c;
            return;
        }
        if (c >= 0x20 && c <= 0x2F)
            return;                     /* 중간 바이트: 무시 */
        v->state = S_GROUND;
        if (c >= 0x40 && c <= 0x7E)
            csiDispatch(v, c);
        return;
    case S_STR:
        if (c == 0x07)
            v->state = S_GROUND;
        else if (c == 0x1B)
            v->state = S_STR_ESC;
        return;
    case S_STR_ESC:
        v->state = c == '\\' ? S_GROUND : S_STR;
        return;
    case S_CHARSET:
        if (v->priv != -1)
            v->lineDrawing = c == '0';
        v->priv = 0;
        v->state = S_GROUND;
        return;
    default:
        break;
    }

    /* S_GROUND */
    if (v->utfNeed > 0) {
        if ((c & 0xC0) == 0x80) {
            v->utf = (v->utf << 6) | (c & 0x3F);
            if (--v->utfNeed == 0)
                putGlyph(v, v->utf > 0xFFFF ? '?' : (int)v->utf);
            return;
        }
        v->utfNeed = 0;
        putGlyph(v, '?');               /* 깨진 UTF-8: 이 바이트는 평소대로 */
    }
    if (c == 0x1B) {
        v->state = S_ESC;
    } else if (c < 0x20) {
        control(v, c);
    } else if (c == 0x7F) {
        /* DEL: 무시 */
    } else if (c < 0x80) {
        putGlyph(v, c);
    } else if ((c & 0xE0) == 0xC0) {
        v->utf = c & 0x1F;
        v->utfNeed = 1;
    } else if ((c & 0xF0) == 0xE0) {
        v->utf = c & 0x0F;
        v->utfNeed = 2;
    } else if ((c & 0xF8) == 0xF0) {
        v->utf = c & 0x07;
        v->utfNeed = 3;
    } else {
        putGlyph(v, '?');
    }
}

void vtWrite(struct vt *v, const char *buf, int n)
{
    int i;

    for (i = 0; i < n; i++)
        feedByte(v, (unsigned char)buf[i]);
}

/* ---------------- 키 -> 바이트 ---------------- */

static int put(char *out, int cap, int n, const char *s)
{
    int len = (int)strlen(s);

    if (n + len > cap)
        return n;
    memcpy(out + n, s, (size_t)len);
    return n + len;
}

int vtEncodeKey(const struct vt *v, int key, char *out, int cap)
{
    int base = KEY_BASE(key);
    int mods = key & KEY_MODS;
    int xm = 1 + ((mods & KEY_MOD_SHIFT) ? 1 : 0) + ((mods & KEY_MOD_ALT) ? 2 : 0) + ((mods & KEY_MOD_CTRL) ? 4 : 0);
    char buf[16];
    int n = 0;

    if (cap < 8)
        return 0;
    if (base == '\t' && (mods & KEY_MOD_SHIFT))
        return put(out, cap, 0, "\033[Z");
    if (base < 0x100) {
        if ((mods & KEY_MOD_ALT) && base != KEY_ESC)
            out[n++] = 0x1B;            /* Alt+글자 = ESC 글자 */
        if (base == ' ' && (mods & KEY_MOD_CTRL))
            out[n++] = 0;               /* Ctrl-Space = NUL */
        else
            out[n++] = (char)base;
        return n;
    }

    switch (base) {
    case KEY_UP: case KEY_DOWN: case KEY_RIGHT: case KEY_LEFT: case KEY_HOME: case KEY_END: {
        char f = base == KEY_UP ? 'A' : base == KEY_DOWN ? 'B' : base == KEY_RIGHT ? 'C' :
                 base == KEY_LEFT ? 'D' : base == KEY_HOME ? 'H' : 'F';

        if (xm > 1)
            snprintf(buf, sizeof(buf), "\033[1;%d%c", xm, f);
        else
            snprintf(buf, sizeof(buf), "\033%c%c", vtAppCursorKeys(v) ? 'O' : '[', f);
        return put(out, cap, 0, buf);
    }
    case KEY_INS: case KEY_DEL: case KEY_PGUP: case KEY_PGDN:
    case KEY_F5: case KEY_F6: case KEY_F7: case KEY_F8: case KEY_F9: case KEY_F10: case KEY_F11: case KEY_F12: {
        int num;

        switch (base) {
        case KEY_INS:  num = 2; break;
        case KEY_DEL:  num = 3; break;
        case KEY_PGUP: num = 5; break;
        case KEY_PGDN: num = 6; break;
        case KEY_F5:   num = 15; break;
        case KEY_F11:  num = 23; break;
        case KEY_F12:  num = 24; break;
        default:       num = 17 + (base - KEY_F6); break;
        }
        if (xm > 1)
            snprintf(buf, sizeof(buf), "\033[%d;%d~", num, xm);
        else
            snprintf(buf, sizeof(buf), "\033[%d~", num);
        return put(out, cap, 0, buf);
    }
    case KEY_F1: case KEY_F2: case KEY_F3: case KEY_F4:
        if (xm > 1)
            snprintf(buf, sizeof(buf), "\033[1;%d%c", xm, 'P' + (base - KEY_F1));
        else
            snprintf(buf, sizeof(buf), "\033O%c", 'P' + (base - KEY_F1));
        return put(out, cap, 0, buf);
    default:
        return 0;
    }
}
