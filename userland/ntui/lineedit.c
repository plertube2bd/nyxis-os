/*
 * lineedit.c - 한 줄 입력 (실행 상자, 화면 잠금의 비밀번호, 파일 탐색기의 입력)
 *
 * 여러 운영체제/셸에서 쓰는 편집 키:
 *   <- -> Home End, Ctrl-A / Ctrl-E (처음/끝), Backspace, Delete,
 *   Ctrl+<- / Ctrl+-> (낱말 단위 이동), Ctrl+Backspace / Ctrl-W (앞 낱말 지우기), Ctrl+Delete (뒤 낱말 지우기),
 *   Ctrl-U (커서 앞 전부 지우기), Ctrl-K (커서 뒤 전부 지우기)
 */
#include <string.h>

#include "ntui.h"

void leInit(struct lineedit *le, char *buf, int cap)
{
    le->buf = buf;
    le->cap = cap;
    le->len = 0;
    le->pos = 0;
    buf[0] = '\0';
}

void leSet(struct lineedit *le, const char *s)
{
    int n = (int)strlen(s);

    if (n > le->cap - 1)
        n = le->cap - 1;
    memcpy(le->buf, s, (size_t)n);
    le->buf[n] = '\0';
    le->len = n;
    le->pos = n;
}

static void cut(struct lineedit *le, int from, int to)
{
    if (from >= to)
        return;
    memmove(le->buf + from, le->buf + to, (size_t)(le->len - to + 1));
    le->len -= to - from;
    le->pos = from;
}

static int wordLeft(const struct lineedit *le)
{
    int p = le->pos;

    while (p > 0 && le->buf[p - 1] == ' ')
        p--;
    while (p > 0 && le->buf[p - 1] != ' ')
        p--;
    return p;
}

static int wordRight(const struct lineedit *le)
{
    int p = le->pos;

    while (p < le->len && le->buf[p] == ' ')
        p++;
    while (p < le->len && le->buf[p] != ' ')
        p++;
    return p;
}

int leKey(struct lineedit *le, int key)
{
    int base = KEY_BASE(key);
    int ctrl = (key & KEY_MOD_CTRL) != 0;

    if (key & (KEY_MOD_ALT | KEY_MOD_SUPER))
        return 0;
    switch (base) {
    case KEY_LEFT:
        le->pos = ctrl ? wordLeft(le) : (le->pos > 0 ? le->pos - 1 : 0);
        return 1;
    case KEY_RIGHT:
        le->pos = ctrl ? wordRight(le) : (le->pos < le->len ? le->pos + 1 : le->len);
        return 1;
    case KEY_HOME:
    case KEY_CTRL('a'):
        le->pos = 0;
        return 1;
    case KEY_END:
    case KEY_CTRL('e'):
        le->pos = le->len;
        return 1;
    case 0x7F:
    case 0x08:
        if (ctrl)
            cut(le, wordLeft(le), le->pos);
        else if (le->pos > 0)
            cut(le, le->pos - 1, le->pos);
        return 1;
    case KEY_CTRL('w'):
        cut(le, wordLeft(le), le->pos);
        return 1;
    case KEY_DEL:
        if (ctrl)
            cut(le, le->pos, wordRight(le));
        else if (le->pos < le->len)
            cut(le, le->pos, le->pos + 1);
        return 1;
    case KEY_CTRL('u'):
        cut(le, 0, le->pos);
        return 1;
    case KEY_CTRL('k'):
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

void leDraw(const struct lineedit *le, int x, int y, int width, int secret, int cursor)
{
    int start = le->pos >= width ? le->pos - width + 1 : 0;
    int c;

    for (c = 0; c < width; c++) {
        int i = start + c;
        int ch = i < le->len ? (secret ? '*' : (unsigned char)le->buf[i]) : ' ';

        screenPut(x + c, y, ch, cursor && i == le->pos ? ROLE_SELECTION : ROLE_WINDOW);
    }
}
