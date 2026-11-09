/*
 * menu.c - ntui 메뉴 (Ctrl-Q, 작업 표시줄 왼쪽의 [ntui])
 *
 * 작업 표시줄 바로 위, 왼쪽에 뜬다. 열려 있는 동안 모든 키를 받는다.
 *   ↑ ↓ 고르기, Enter 실행, Esc 또는 Ctrl-Q 닫기. 항목 옆의 글자(밑줄 대신 [ ])를 눌러도 된다.
 *
 *   Apps    New terminal [t] / Files [f] / Run... [r]
 *   System  Change theme [c] / Help [h] / Shell [s]  (nysh 를 전체 화면으로. exit 하면 ntui 로 돌아온다)
 *   Power   Log out [l]  (ntui 를 끝낸다: ntui 가 로그인 셸이면 로그인 화면으로)
 *           Power off [p] (init 에게 전원 끄기를 요청한다: app:/poweroff.run)
 * Log out / Power off 는 한 번 더 묻는다 (Enter 또는 y = 예).
 * 다시 시작(reboot)은 아직 없다: 커널/init 에 재시작 기능이 없다.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "ntui.h"

enum action {
    A_NONE, A_TERMINAL, A_FILES, A_RUN, A_THEME, A_HELP, A_SHELL, A_LOGOUT, A_POWEROFF
};

struct item {
    const char *label;                  /* NULL = 구분선. section 이 있으면 제목줄 */
    const char *section;
    char key;                           /* 바로 고르는 글자 */
    const char *hint;                   /* 오른쪽에 보이는 단축키 */
    enum action action;
};

static const struct item g_items[] = {
    { NULL, "Apps", 0, NULL, A_NONE },
    { "New terminal", NULL, 't', "^T", A_TERMINAL },
    { "Files", NULL, 'f', "^E", A_FILES },
    { "Run...", NULL, 'r', "^R", A_RUN },
    { NULL, "System", 0, NULL, A_NONE },
    { "Change theme", NULL, 'c', "^G", A_THEME },
    { "Help", NULL, 'h', NULL, A_HELP },
    { "Shell (nysh)", NULL, 's', NULL, A_SHELL },
    { NULL, "Power", 0, NULL, A_NONE },
    { "Log out", NULL, 'l', NULL, A_LOGOUT },
    { "Power off", NULL, 'p', NULL, A_POWEROFF },
};

#define ITEM_COUNT ((int)(sizeof(g_items) / sizeof(g_items[0])))
#define MENU_W 30

static bool g_open;
static int g_sel = 1;
static enum action g_confirm;           /* 확인을 기다리는 동작 (A_NONE = 없음) */
static char g_msg[48];

static bool selectable(int i)
{
    return i >= 0 && i < ITEM_COUNT && g_items[i].label != NULL;
}

void menuOpen(void)
{
    g_open = true;
    g_confirm = A_NONE;
    g_msg[0] = '\0';
    if (!selectable(g_sel))
        g_sel = 1;
}

void menuClose(void)
{
    g_open = false;
    g_confirm = A_NONE;
}

int menuIsOpen(void)
{
    return g_open;
}

void menuDraw(void)
{
    int h = ITEM_COUNT + 2 + 2;         /* 테두리 + 아래 안내 두 줄 */
    int x = 0;
    int y = screenRows - TASKBAR_ROWS - h;
    int w = MENU_W < screenCols ? MENU_W : screenCols;
    char line[64];

    if (!g_open)
        return;
    if (y < 0)
        y = 0;
    screenFill(x, y, w, h, ' ', ROLE_WINDOW);
    screenBox(x, y, w, h, ROLE_BORDER);
    screenFill(x + 1, y, w - 2, 1, ' ', ROLE_TITLE_ACTIVE);
    snprintf(line, sizeof(line), "ntui  %s@%s", username, hostname);
    line[w - 4 > 0 ? w - 4 : 0] = '\0';
    screenText(x + 2, y, line, ROLE_TITLE_ACTIVE);

    for (int i = 0; i < ITEM_COUNT; i++) {
        const struct item *it = &g_items[i];
        int yy = y + 1 + i;

        if (it->label == NULL) {
            screenText(x + 2, yy, it->section, ROLE_BORDER);
            continue;
        }
        enum role r = i == g_sel ? ROLE_SELECTION : ROLE_WINDOW;

        if (i == g_sel)
            screenFill(x + 1, yy, w - 2, 1, ' ', r);
        snprintf(line, sizeof(line), "  [%c] %s", it->key, it->label);
        screenText(x + 1, yy, line, r);
        if (it->hint != NULL)
            screenText(x + w - 2 - (int)strlen(it->hint), yy, it->hint, i == g_sel ? r : ROLE_BORDER);
    }

    if (g_confirm == A_LOGOUT)
        screenText(x + 2, y + h - 2, "Log out? Enter/y = yes", ROLE_ERROR);
    else if (g_confirm == A_POWEROFF)
        screenText(x + 2, y + h - 2, "Power off? Enter/y = yes", ROLE_ERROR);
    else if (g_msg[0] != '\0')
        screenText(x + 2, y + h - 2, g_msg, ROLE_ERROR);
    else
        screenText(x + 2, y + h - 2, "Enter select  Esc close", ROLE_BORDER);
}

/* 전원 끄기: init 에게 요청한다. 성공하면 곧 꺼지므로 돌아오지 않는 것이 보통이다 */
static void powerOff(void)
{
    wmCloseAll();                       /* 터미널에서 돌던 명령을 먼저 끝낸다 */
    if (nrunnerExecEx("poweroff", 0, "ntui: powering off...") == 0) {
        sleep(5);                       /* init 이 끄는 동안 */
    }
    snprintf(g_msg, sizeof(g_msg), "power off failed");
    g_open = true;
}

static int perform(enum action a)
{
    menuClose();
    switch (a) {
    case A_TERMINAL:
        (void)nterminalOpen();
        break;
    case A_FILES:
        (void)nfilesOpen(NULL);
        break;
    case A_RUN:
        nrunnerOpen();
        break;
    case A_THEME:
        themeNext();
        (void)themeSave();
        screenInvalidate();
        g_open = true;                  /* 메뉴를 둔 채로 테마를 바꿔 볼 수 있게 */
        break;
    case A_HELP:
        helpOpen();
        break;
    case A_SHELL:
        (void)nrunnerExecEx("nysh", 0, "ntui: nysh - type `exit` to return to ntui");
        break;
    case A_LOGOUT:
        return MENU_QUIT;
    case A_POWEROFF:
        powerOff();
        break;
    default:
        break;
    }
    return MENU_DONE;
}

/* 열려 있으면 모든 키를 먹는다. 돌려주는 값: MENU_IGNORED(닫혀 있다), MENU_DONE, MENU_QUIT(ntui 를 끝낸다) */
int menuKey(int key)
{
    if (!g_open)
        return MENU_IGNORED;

    if (g_confirm != A_NONE) {
        enum action a = g_confirm;

        g_confirm = A_NONE;
        if (key == '\r' || key == '\n' || key == 'y' || key == 'Y')
            return perform(a);
        return MENU_DONE;               /* 그 밖의 키 = 아니오 */
    }
    g_msg[0] = '\0';

    switch (key) {
    case KEY_ESC:
    case KEY_CTRL('q'):
        menuClose();
        return MENU_DONE;
    case KEY_UP:
        do
            g_sel = (g_sel + ITEM_COUNT - 1) % ITEM_COUNT;
        while (!selectable(g_sel));
        return MENU_DONE;
    case KEY_DOWN:
    case '\t':
        do
            g_sel = (g_sel + 1) % ITEM_COUNT;
        while (!selectable(g_sel));
        return MENU_DONE;
    case '\r':
    case '\n':
        key = g_items[g_sel].key;
        break;
    }

    for (int i = 0; i < ITEM_COUNT; i++) {
        if (selectable(i) && g_items[i].key == key) {
            g_sel = i;
            if (g_items[i].action == A_LOGOUT || g_items[i].action == A_POWEROFF) {
                g_confirm = g_items[i].action;
                return MENU_DONE;
            }
            return perform(g_items[i].action);
        }
    }
    return MENU_DONE;
}
