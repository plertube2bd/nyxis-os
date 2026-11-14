/*
 * lineedit.c - 한 줄 입력 그리기 (실행 상자, 화면 잠금의 비밀번호). 편집 규칙은 nwp.h 의 nwp_le_key (창 프로그램과 같다)
 *
 * 여러 운영체제/셸에서 쓰는 편집 키:
 *   <- -> Home End, Ctrl-A / Ctrl-E (처음/끝), Backspace, Delete,
 *   Ctrl+<- / Ctrl+-> (낱말 단위 이동), Ctrl+Backspace / Ctrl-W (앞 낱말 지우기), Ctrl+Delete (뒤 낱말 지우기),
 *   Ctrl-U (커서 앞 전부 지우기), Ctrl-K (커서 뒤 전부 지우기)
 */
#include <string.h>

#include "ntui.h"

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
