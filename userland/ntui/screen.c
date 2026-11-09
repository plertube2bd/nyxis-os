/*
 * screen.c - 화면: 셀 버퍼 두 벌(그릴 것 / 화면에 있는 것)과 바뀐 칸만 내보내기
 *
 * 그리는 함수는 g_back 에만 쓴다. screenFlush 가 g_front 와 비교해 다른 칸만 커서 이동 + 색 + 글자로
 * 내보내고, 출력은 g_out 에 모아 write 로 한꺼번에 보낸다 (시리얼 115200bps 에서 차이가 크다).
 * 셀은 글자와 역할(ROLE_*)을 가진다: 실제 색은 테마가 정하므로 테마를 바꾸면 screenInvalidate 후 다시 그린다.
 *
 * 콘솔 제약 (nxkernel/console/outputs/ansi.h): 글자는 ASCII(0x20-0x7E)만, 대체 화면/스크롤 영역 없음.
 * 줄 끝 칸을 쓰면 다음 글자를 쓸 때 줄이 바뀌므로(오른쪽 아래 칸도 스크롤하지 않는다) 끝 칸을 쓴 뒤에는
 * 커서 위치를 모른다고 보고 다음 칸에서 다시 옮긴다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "ntui.h"

#define OUT_MAX 4096

struct cell {
    char ch;
    unsigned char role;
};

int screenCols;
int screenRows;
int screenColors = 256;                 /* 16 이면 테마의 16색 값을 쓴다 */

static struct cell *g_back;             /* 다음 프레임 */
static struct cell *g_front;            /* 화면에 지금 있는 것 */
static char g_out[OUT_MAX];
static size_t g_outLen;

/* ---------------- 출력 버퍼 ---------------- */

static void outFlush(void)
{
    size_t off = 0;

    while (off < g_outLen) {
        ssize_t n = write(ttyFd(), g_out + off, g_outLen - off);

        if (n <= 0)
            break;                      /* 콘솔이 사라졌다: 버린다 */
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

static int clampChar(int ch)
{
    return (ch >= 0x20 && ch <= 0x7E) ? ch : '?';
}

/* 화면 크기를 읽고 버퍼를 만든다. 커서를 숨기고 화면을 지운다 */
int screenInit(void)
{
    struct winsize ws;
    size_t n;

    if (ttyOpen() < 0)
        return -1;
    screenCols = 80;
    screenRows = 24;
    if (ioctl(ttyFd(), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        screenCols = ws.ws_col;
        screenRows = ws.ws_row;
    }

    n = (size_t)screenCols * (size_t)screenRows;
    free(g_back);
    free(g_front);
    g_back = (struct cell *)malloc(n * sizeof(struct cell));
    g_front = (struct cell *)malloc(n * sizeof(struct cell));
    if (g_back == NULL || g_front == NULL) {
        free(g_back);
        free(g_front);
        g_back = g_front = NULL;
        return -1;
    }

    outCstr("\033[?25l");               /* 커서 숨기기 */
    screenClear(ROLE_DESKTOP);
    screenInvalidate();
    return 0;
}

/* 색/커서를 되돌리고 화면을 지운다 (셸로 돌아갈 때) */
void screenEnd(void)
{
    outCstr("\033[0m\033[2J\033[H\033[?25h");
    outFlush();
    free(g_back);
    free(g_front);
    g_back = g_front = NULL;
}

/* 화면을 다른 프로그램에 잠시 넘긴다: 색/커서를 되돌리고 지운다 (버퍼는 그대로) */
void screenSuspend(void)
{
    outCstr("\033[0m\033[2J\033[H\033[?25h");
    outFlush();
}

/* 다시 가져온다: 커서를 숨기고 다음 screenFlush 가 모든 칸을 다시 그리게 한다 */
void screenResume(void)
{
    outCstr("\033[?25l");
    screenInvalidate();
}

/* ---------------- 그리기 (g_back 에만) ---------------- */

void screenPut(int x, int y, int ch, enum role r)
{
    struct cell *c;

    if (g_back == NULL || x < 0 || y < 0 || x >= screenCols || y >= screenRows)
        return;
    c = &g_back[y * screenCols + x];
    c->ch = (char)clampChar(ch);
    c->role = (unsigned char)r;
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

/* ASCII 테두리 (+ - |). 안쪽은 건드리지 않는다. w, h 는 테두리 포함 크기 */
void screenBox(int x, int y, int w, int h, enum role r)
{
    int i;

    if (w < 2 || h < 2)
        return;
    for (i = 1; i < w - 1; i++) {
        screenPut(x + i, y, '-', r);
        screenPut(x + i, y + h - 1, '-', r);
    }
    for (i = 1; i < h - 1; i++) {
        screenPut(x, y + i, '|', r);
        screenPut(x + w - 1, y + i, '|', r);
    }
    screenPut(x, y, '+', r);
    screenPut(x + w - 1, y, '+', r);
    screenPut(x, y + h - 1, '+', r);
    screenPut(x + w - 1, y + h - 1, '+', r);
}

/* ---------------- 내보내기 ---------------- */

/* 다음 screenFlush 가 모든 칸을 다시 보내게 한다 (테마 변경, 화면이 다른 출력으로 망가졌을 때) */
void screenInvalidate(void)
{
    size_t i;
    size_t n = (size_t)screenCols * (size_t)screenRows;

    if (g_front == NULL)
        return;
    for (i = 0; i < n; i++) {
        g_front[i].ch = 0;              /* 그릴 수 있는 글자가 아니므로 늘 다르다 */
        g_front[i].role = 0xFF;
    }
}

/* g_back 과 g_front 의 다른 칸만 보내고 g_front 를 맞춘다 */
void screenFlush(void)
{
    char seq[48];
    int x;
    int y;
    int curX = -1;                      /* 터미널 커서 위치 (-1 = 모름) */
    int curY = -1;
    int curRole = -1;                   /* 마지막으로 보낸 색 (-1 = 모름) */

    if (g_back == NULL)
        return;
    for (y = 0; y < screenRows; y++) {
        for (x = 0; x < screenCols; x++) {
            struct cell *b = &g_back[y * screenCols + x];
            struct cell *f = &g_front[y * screenCols + x];

            if (b->ch == f->ch && b->role == f->role)
                continue;
            if (x != curX || y != curY) {
                snprintf(seq, sizeof(seq), "\033[%d;%dH", y + 1, x + 1);
                outCstr(seq);
            }
            if ((int)b->role != curRole) {
                themeSgr((enum role)b->role, screenColors, seq, sizeof(seq));
                outCstr(seq);
                curRole = b->role;
            }
            outStr(&b->ch, 1);
            *f = *b;
            curX = x + 1;
            curY = y;
            if (curX >= screenCols)
                curX = -1;              /* 줄 끝: 다음 글자에서 줄이 바뀌므로 위치를 모른다고 본다 */
        }
    }
    outFlush();
}
