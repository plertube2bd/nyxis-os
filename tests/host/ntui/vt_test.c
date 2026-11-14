/*
 * vt_test.c - nterm 터미널 에뮬레이터(userland/nterm/vt.c) 호스트 검사
 *
 * vt.c 는 libc 만 쓰므로 호스트 컴파일러 + 새니타이저로 바로 돌린다 (make test-ntui).
 * 프로그램 출력 바이트를 넣고 격자(글자, 색, 커서)와 키 인코딩, 응답(커서 위치 보고)을 확인한다.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "vt.h"

static int g_fail;
static int g_count;
static char g_reply[64];

static void check(int ok, const char *what)
{
    g_count++;
    if (!ok) {
        g_fail++;
        printf("FAIL %s\n", what);
    }
}

static void onReply(void *ctx, const char *s, int n)
{
    (void)ctx;
    if (n >= (int)sizeof(g_reply))
        n = (int)sizeof(g_reply) - 1;
    memcpy(g_reply, s, (size_t)n);
    g_reply[n] = '\0';
}

static void feed(struct vt *v, const char *s)
{
    vtWrite(v, s, (int)strlen(s));
}

/* 줄 y 의 글자들 (끝 공백 제외) */
static const char *text(const struct vt *v, int y, int scroll)
{
    static char buf[300];
    const struct vtcell *line = vtLine(v, y, scroll);
    int n = vtCols(v);
    int i;

    for (i = 0; i < n; i++)
        buf[i] = line[i].ch < 0x80 ? (char)line[i].ch : '#';
    while (n > 0 && buf[n - 1] == ' ')
        n--;
    buf[n] = '\0';
    return buf;
}

static int cursorAt(const struct vt *v, int x, int y)
{
    int cx;
    int cy;

    (void)vtCursor(v, &cx, &cy);
    return cx == x && cy == y;
}

static void testBasic(void)
{
    struct vt *v = vtNew(10, 4, onReply, NULL);

    feed(v, "hello\r\nworld");
    check(strcmp(text(v, 0, 0), "hello") == 0, "basic line 0");
    check(strcmp(text(v, 1, 0), "world") == 0, "basic line 1");
    check(cursorAt(v, 5, 1), "basic cursor");
    /* LF 만으로도 줄 처음 (Nyxis 콘솔과 같다) */
    feed(v, "\nx");
    check(strcmp(text(v, 2, 0), "x") == 0, "LF returns to column 0");
    /* 탭 */
    feed(v, "\r\n\tT");
    check(vtLine(v, 3, 0)[8].ch == 'T', "tab stop 8");
    vtFree(v);
}

static void testWrapAndScroll(void)
{
    struct vt *v = vtNew(5, 3, onReply, NULL);

    feed(v, "abcde");
    check(cursorAt(v, 4, 0), "pending wrap keeps cursor on last column");
    feed(v, "f");
    check(strcmp(text(v, 1, 0), "f") == 0, "wrap to next line");
    feed(v, "\r\n2\r\n3\r\n4");
    check(strcmp(text(v, 0, 0), "2") == 0, "scrolled up");
    check(vtScrollback(v) == 2, "two lines in scrollback");
    check(strcmp(text(v, 0, 2), "abcde") == 0, "scrollback line visible");
    /* BS 가 줄 처음에서 윗줄 끝으로 */
    feed(v, "\r\b");
    check(cursorAt(v, 4, 1), "BS reverse-wraps");
    vtFree(v);
}

static void testCsi(void)
{
    struct vt *v = vtNew(20, 5, onReply, NULL);

    feed(v, "\033[2J\033[3;5HX");
    check(vtLine(v, 2, 0)[4].ch == 'X', "CUP");
    feed(v, "\033[1;1Habcdef\033[1;3H\033[2P");
    check(strcmp(text(v, 0, 0), "abef") == 0, "DCH");
    feed(v, "\033[2@");
    check(strcmp(text(v, 0, 0), "ab  ef") == 0, "ICH");
    feed(v, "\033[K");
    check(strcmp(text(v, 0, 0), "ab") == 0, "EL");
    feed(v, "\033[6n");
    check(strcmp(g_reply, "\033[1;3R") == 0, "CPR reply");
    feed(v, "\033[c");
    check(strcmp(g_reply, "\033[?1;2c") == 0, "DA reply");
    /* 스크롤 영역 + IL/DL */
    feed(v, "\033[2J\033[1;1H1\r\n2\r\n3\r\n4\r\n5");
    feed(v, "\033[2;4r\033[2;1H\033[L");
    check(strcmp(text(v, 1, 0), "") == 0 && strcmp(text(v, 2, 0), "2") == 0 &&
          strcmp(text(v, 3, 0), "3") == 0 && strcmp(text(v, 4, 0), "5") == 0, "IL inside region");
    feed(v, "\033[M");
    check(strcmp(text(v, 1, 0), "2") == 0 && strcmp(text(v, 3, 0), "") == 0, "DL inside region");
    feed(v, "\033[r");
    vtFree(v);
}

static void testSgrAndAlt(void)
{
    struct vt *v = vtNew(10, 3, onReply, NULL);
    const struct vtcell *c;

    feed(v, "\033[1;31mR\033[0m\033[38;5;200mP\033[48;2;255;0;0mQ\033[7mZ");
    c = vtLine(v, 0, 0);
    check(c[0].fg == 1 && (c[0].attr & VT_ATTR_BOLD) && !(c[0].attr & VT_ATTR_FGDEF), "SGR red bold");
    check(c[1].fg == 200 && (c[1].attr & VT_ATTR_BGDEF), "SGR 256");
    check(c[2].bg == 196 && !(c[2].attr & VT_ATTR_BGDEF), "SGR truecolor -> 196");
    check(c[3].attr & VT_ATTR_REVERSE, "SGR reverse");

    feed(v, "\033[0m\033[?1049h");
    check(strcmp(text(v, 0, 0), "") == 0, "alt screen is clear");
    feed(v, "alt");
    feed(v, "\033[?1049l");
    check(strcmp(text(v, 0, 0), "RPQZ") == 0, "main screen restored");
    check(cursorAt(v, 4, 0), "cursor restored after 1049");
    feed(v, "\033[?25l");
    {
        int x;
        int y;

        check(vtCursor(v, &x, &y) == 0, "cursor hidden");
    }
    vtFree(v);
}

static void testUtf8AndLineDrawing(void)
{
    struct vt *v = vtNew(10, 2, onReply, NULL);
    const struct vtcell *c;

    feed(v, "\xe2\x94\x8c\xc3\xa9\033(0q\033(Bq");
    c = vtLine(v, 0, 0);
    check(c[0].ch == 0x250C, "UTF-8 box drawing kept");
    check(c[1].ch == '?', "other UTF-8 becomes ?");
    check(c[2].ch == 0x2500, "DEC line drawing");
    check(c[3].ch == 'q', "back to ASCII");
    /* OSC 는 버린다 */
    feed(v, "\033]0;title\007ok");
    check(c[4].ch == 'o' && c[5].ch == 'k', "OSC skipped");
    vtFree(v);
}

static void testResize(void)
{
    struct vt *v = vtNew(10, 4, onReply, NULL);

    feed(v, "1\r\n2\r\n3\r\n4");
    vtResize(v, 6, 2);
    check(vtCols(v) == 6 && vtRows(v) == 2, "resized");
    check(strcmp(text(v, 1, 0), "4") == 0, "cursor line kept visible");
    check(cursorAt(v, 1, 1), "cursor moved with content");
    check(vtScrollback(v) == 2, "cut lines go to scrollback");
    vtResize(v, 8, 5);
    check(strcmp(text(v, 0, 0), "3") == 0 && strcmp(text(v, 1, 0), "4") == 0, "grow keeps content");
    vtFree(v);
}

static void testKeys(void)
{
    struct vt *v = vtNew(10, 2, onReply, NULL);
    char b[16];
    int n;

    n = vtEncodeKey(v, 'a', b, sizeof(b));
    check(n == 1 && b[0] == 'a', "plain key");
    n = vtEncodeKey(v, 'x' | KEY_MOD_ALT, b, sizeof(b));
    check(n == 2 && b[0] == 0x1B && b[1] == 'x', "Alt+x");
    n = vtEncodeKey(v, KEY_UP, b, sizeof(b));
    check(n == 3 && memcmp(b, "\033[A", 3) == 0, "Up");
    feed(v, "\033[?1h");
    n = vtEncodeKey(v, KEY_UP, b, sizeof(b));
    check(n == 3 && memcmp(b, "\033OA", 3) == 0, "Up in application cursor mode");
    n = vtEncodeKey(v, KEY_RIGHT | KEY_MOD_CTRL, b, sizeof(b));
    check(n == 6 && memcmp(b, "\033[1;5C", 6) == 0, "Ctrl+Right");
    n = vtEncodeKey(v, KEY_DEL, b, sizeof(b));
    check(n == 4 && memcmp(b, "\033[3~", 4) == 0, "Delete");
    n = vtEncodeKey(v, '\t' | KEY_MOD_SHIFT, b, sizeof(b));
    check(n == 3 && memcmp(b, "\033[Z", 3) == 0, "Shift+Tab");
    n = vtEncodeKey(v, KEY_F5, b, sizeof(b));
    check(n == 5 && memcmp(b, "\033[15~", 5) == 0, "F5");
    n = vtEncodeKey(v, KEY_CTRL('c'), b, sizeof(b));
    check(n == 1 && b[0] == 3, "Ctrl-C byte");
    vtFree(v);
}

/* 무작위 바이트로 터지지 않는지 (새니타이저가 메모리 오류를 잡는다) */
static void testFuzz(void)
{
    struct vt *v = vtNew(13, 7, onReply, NULL);
    char buf[64];
    int i;
    int k;

    srand(1);
    for (i = 0; i < 20000; i++) {
        for (k = 0; k < (int)sizeof(buf); k++) {
            int r = rand() % 10;

            buf[k] = r < 3 ? "\033[;?0123456789HJKLMPm@hlrABCDsu"[rand() % 30] : (char)(rand() & 0xFF);
        }
        vtWrite(v, buf, (int)sizeof(buf));
        if (i % 1000 == 0)
            vtResize(v, 2 + rand() % 40, 1 + rand() % 20);
    }
    check(1, "fuzz survived");
    vtFree(v);
}

/* 창 조작 (CSI t), XTVERSION, OSC 제목 */
static int g_ops[8][4];
static int g_nops;
static char g_title[64];

static void onWinop(void *ctx, const int *p, int n)
{
    int i;

    (void)ctx;
    if (g_nops >= 8)
        return;
    for (i = 0; i < 4; i++)
        g_ops[g_nops][i] = i < n ? p[i] : -1;
    g_nops++;
}

static void onTitle(void *ctx, const char *t)
{
    (void)ctx;
    snprintf(g_title, sizeof(g_title), "%s", t);
}

static void testWindowOps(void)
{
    struct vt *v = vtNew(20, 5, onReply, NULL);

    vtSetWindowOps(v, onWinop, onTitle);
    g_nops = 0;
    feed(v, "\033[8;30;100t\033[3;5;2tA\033[10;1t\033[18t");
    check(g_nops == 4, "four window ops");
    check(g_ops[0][0] == 8 && g_ops[0][1] == 30 && g_ops[0][2] == 100, "resize params (rows;cols)");
    check(g_ops[1][0] == 3 && g_ops[1][1] == 5 && g_ops[1][2] == 2, "move params");
    check(g_ops[2][0] == 10 && g_ops[2][1] == 1 && g_ops[2][3] == -1, "fullscreen params");
    check(g_ops[3][0] == 18 && g_ops[3][1] == -1, "size query");
    check(strncmp(text(v, 0, 0), "A", 1) == 0, "text between window ops is printed");
    g_nops = 0;
    feed(v, "\033[2 t\033[ q");      /* DECSWBV / DECSCUSR: 중간 바이트가 있으면 창 조작이 아니다 */
    check(g_nops == 0, "intermediate bytes are not window ops");
    g_reply[0] = '\0';
    feed(v, "\033[>0q");
    check(strcmp(g_reply, "\033P>|nterm(1)\033\\") == 0, "XTVERSION reply");
    feed(v, "\033]2;hello\x01world\007");
    check(strcmp(g_title, "hello?world") == 0, "OSC 2 title (BEL)");
    feed(v, "\033]0;two\033\\B");
    check(strcmp(g_title, "two") == 0, "OSC 0 title (ST)");
    check(strncmp(text(v, 0, 0), "AB", 2) == 0, "printing continues after OSC");
    g_title[0] = '\0';
    feed(v, "\033]52;c;Zm9v\007\033Pfoo;2;bar\033\\");
    check(g_title[0] == '\0', "other OSC / DCS strings are not titles");
    vtFree(v);
}

int main(void)
{
    testBasic();
    testWrapAndScroll();
    testCsi();
    testSgrAndAlt();
    testUtf8AndLineDrawing();
    testResize();
    testKeys();
    testWindowOps();
    testFuzz();
    printf("vt: %d/%d checks passed\n", g_count - g_fail, g_count);
    return g_fail ? 1 : 0;
}
