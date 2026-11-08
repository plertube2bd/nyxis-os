/*
 * main.c - ntui 진입점
 *
 * 지금은 screen.c 확인용 데모: 바탕화면 + 창 하나. Ctrl-T 테마 바꾸기(저장), q 끝.
 */
#include <stdio.h>

#include "ntui.h"

#define KEY_CTRL_T 0x14

static void draw(void)
{
    int w = screenCols < 44 ? screenCols : 44;
    int h = screenRows < 9 ? screenRows : 9;
    int x = (screenCols - w) / 2;
    int y = (screenRows - h) / 2;
    char line[64];

    screenClear(ROLE_DESKTOP);

    screenFill(x, y, w, h, ' ', ROLE_WINDOW);
    screenBox(x, y, w, h, ROLE_BORDER);
    screenFill(x + 1, y, w - 2, 1, ' ', ROLE_TITLE_ACTIVE);
    screenText(x + 2, y, "ntui", ROLE_TITLE_ACTIVE);

    snprintf(line, sizeof(line), "user  %s%s", username, isRoot ? " (root)" : "");
    screenText(x + 2, y + 2, line, ROLE_WINDOW);
    snprintf(line, sizeof(line), "zone  %s", timezoneName);
    screenText(x + 2, y + 3, line, ROLE_WINDOW);
    snprintf(line, sizeof(line), "theme %s", currentTheme->name);
    screenText(x + 2, y + 4, line, ROLE_SELECTION);
    screenText(x + 2, y + 6, "Ctrl-T theme   q quit", ROLE_WINDOW);
}

int main(void)
{
    int key;

    if (init() < 0)
        return 1;
    themeLoad();
    if (screenInit() < 0) {
        fprintf(stderr, "ntui: screen init failed\n");
        return 1;
    }

    for (;;) {
        draw();
        screenFlush();
        key = getch();
        if (key == EOF || key == 'q')
            break;
        if (key == KEY_CTRL_T) {
            themeNext();
            (void)themeSave();
            screenInvalidate();         /* 색만 바뀐 칸도 다시 보내야 한다 */
        }
    }

    screenEnd();
    return 0;
}
