/*
 * switcher.c - 창 전환기 (Alt+Tab, Super+Tab)
 *
 * 화면 가운데에 창 목록을 띄우고 하나를 고른다.
 *   Alt+Tab        다음 창이 골라진 채로 열린다. Alt 를 누른 채 Tab 을 더 누르면 다음, Shift+Tab 은 이전.
 *                  Alt 를 떼면 고른 창으로 간다 (Windows / GNOME 과 같다). Alt 뗌을 모르는 터미널 모드에서는
 *                  열지 않고 바로 다음 창으로 간다 (main.c).
 *   Super+Tab      Alt 를 떼도 닫히지 않는다 (작업 보기): ← → ↑ ↓ Tab 으로 고르고 Enter, 1..9 바로 고르기.
 *   Esc            그대로 닫는다.   Del / w  고른 창 닫기.
 */
#include <stdio.h>
#include <string.h>

#include "ntui.h"

static int g_open;
static int g_sel;
static int g_holdAlt;

static int count(void)
{
    return wmCount();
}

void switcherOpen(int step, int holdAlt)
{
    int n = count();

    if (n == 0)
        return;
    g_open = 1;
    g_holdAlt = holdAlt;
    g_sel = wmActiveIndex();
    if (g_sel < 0)
        g_sel = step > 0 ? n - 1 : 0;
    g_sel = (g_sel + step + n) % n;
    inputWantAltRelease(holdAlt);
}

int switcherIsOpen(void)
{
    return g_open;
}

static void finish(int choose)
{
    g_open = 0;
    inputWantAltRelease(0);
    if (choose && g_sel >= 0 && g_sel < count())
        wmSetActive(g_sel);
}

void switcherDraw(void)
{
    int n = count();
    int w = 44;
    int h;
    int x;
    int y;
    int i;
    char line[64];

    if (!g_open)
        return;
    if (g_sel >= n)
        g_sel = n - 1;
    if (n == 0) {
        finish(0);
        return;
    }
    h = n + 4;
    if (w > screenCols)
        w = screenCols;
    x = (screenCols - w) / 2;
    y = (screenRows - TASKBAR_ROWS - h) / 2;
    if (y < 0)
        y = 0;
    screenFill(x, y, w, h, ' ', ROLE_WINDOW);
    screenBox(x, y, w, h, ROLE_BORDER);
    screenFill(x + 1, y, w - 2, 1, ' ', ROLE_TITLE_ACTIVE);
    screenText(x + 2, y, "Switch window", ROLE_TITLE_ACTIVE);
    for (i = 0; i < n; i++) {
        enum role r = i == g_sel ? ROLE_SELECTION : ROLE_WINDOW;

        snprintf(line, sizeof(line), " %d  %s%s%s", i + 1, wmIsMinimized(i) ? "(" : "", wmTitle(i),
                 wmIsMinimized(i) ? ")" : "");
        screenFill(x + 1, y + 1 + i, w - 2, 1, ' ', r);
        screenText(x + 1, y + 1 + i, line, r);
    }
    screenText(x + 2, y + h - 2, g_holdAlt ? "Tab next  release Alt to switch" : "Enter switch  Esc cancel",
               ROLE_BORDER);
}

/* 열려 있으면 모든 키를 먹는다 */
int switcherKey(int key)
{
    int n = count();
    int base = KEY_BASE(key);

    if (!g_open)
        return 0;
    if (n == 0) {
        finish(0);
        return 1;
    }
    if (key == KEY_ALT_UP) {
        if (g_holdAlt)
            finish(1);
        return 1;
    }
    switch (base) {
    case '\t':
        g_sel = (g_sel + ((key & KEY_MOD_SHIFT) ? n - 1 : 1)) % n;
        return 1;
    case KEY_RIGHT:
    case KEY_DOWN:
        g_sel = (g_sel + 1) % n;
        return 1;
    case KEY_LEFT:
    case KEY_UP:
        g_sel = (g_sel + n - 1) % n;
        return 1;
    case '\r':
        finish(1);
        return 1;
    case KEY_ESC:
        finish(0);
        return 1;
    case KEY_DEL:
    case 'w':
        wmSetActive(g_sel);
        wmCloseActive();
        finish(0);
        return 1;
    default:
        break;
    }
    if (base >= '1' && base <= '9' && base - '1' < n) {
        g_sel = base - '1';
        finish(1);
    }
    return 1;
}
