/*
 * wm.c - 창 관리: 창 목록, 테두리/제목줄, 포커스, 최대화, 앱에 키와 이벤트 넘기기
 *
 * 창 영역은 화면에서 작업 표시줄(맨 아래 TASKBAR_ROWS 줄)을 뺀 곳이다.
 * 창 목록의 순서는 만든 순서 그대로다 (작업 표시줄의 순서). 겹칠 때는 활성 창을 맨 위에 그린다.
 * 앱(struct app)은 창 안쪽(테두리 안)만 그린다. 앱이 창을 닫고 싶으면 w->wantClose = 1 로 두면
 * 다음 wmReap 이 닫는다.
 *
 * 창 동작(닫기, 최대화, 최소화, 옮기기)은 main.c 의 전역 단축키가 wmCloseActive 등으로 부른다.
 * wmKey 는 옮기기 모드의 키와, 그 밖에는 활성 창의 앱에 키를 넘긴다.
 *
 * 최소화: 창을 그리지 않고 키도 주지 않는다 (앱은 계속 돈다: 터미널의 명령, poll, tick). 작업 표시줄에는
 *   "(이름)" 으로 남는다. 활성 창은 늘 최소화되지 않은 창이다: 최소화하면 맨 위의 다른 보이는 창이 활성이 되고,
 *   없으면 활성 창이 없다 (-1). wmSetActive 로 최소화된 창을 고르면 (Ctrl-N/P, 작업 표시줄 Enter) 되살린다.
 *
 * 배치 (w->layout): 보통, 최대화(창 영역 전체), 왼쪽/오른쪽 절반(스냅), 전체 화면(테두리 없이 화면 전체, 작업 표시줄도
 *   가린다). 보통이 아닌 배치로 갈 때 보통 배치의 자리를 기억해 두고, 보통으로 돌아오면 되살린다 (wmSetLayout).
 *
 * 창 메뉴 (wmMenuOpen: Alt+Space, 메뉴 키): 되돌리기/옮기기/크기/최소화/최대화/전체 화면/닫기. 글자 키나 ↑↓ Enter.
 *
 * 옮기기 모드 (wmStartMove): 방향키로 활성 창을 한 칸씩 옮긴다 (PgUp/PgDn 은 위/아래로 크게, Home/End 는 왼쪽/오른쪽 끝).
 *   Tab 을 누르면 크기 조절 모드: -> / <- 로 넓게/좁게, ↓ / ↑ 로 높게/낮게. 다시 Tab 이면 옮기기.
 *   Enter, Esc 로 끝낸다. 모드 중에는 키가 앱으로 가지 않는다. 최대화된 창은 먼저 되돌린다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntui.h"

static struct window *g_wins[WM_MAX];
static int g_count;
static int g_active = -1;

#define MODE_NONE   0
#define MODE_MOVE   1
#define MODE_RESIZE 2
static int g_mode;                      /* 활성 창의 옮기기/크기 조절 모드 */

/* ---------------- 배치 ---------------- */

static void applyLayout(struct window *w);

static int areaRows(void)
{
    return screenRows - TASKBAR_ROWS;
}

/* 창을 창 영역 안으로 (크기를 줄이고, 위치를 당긴다) */
static void clampWindow(struct window *w)
{
    if (w->w > screenCols)
        w->w = screenCols;
    if (w->h > areaRows())
        w->h = areaRows();
    if (w->w < 12)
        w->w = 12;
    if (w->h < 4)
        w->h = 4;
    if (w->x + w->w > screenCols)
        w->x = screenCols - w->w;
    if (w->y + w->h > areaRows())
        w->y = areaRows() - w->h;
    if (w->x < 0)
        w->x = 0;
    if (w->y < 0)
        w->y = 0;
}

/* 활성으로 둘 창: 맨 위(목록의 뒤)부터 최소화되지 않은 첫 창. 없으면 -1 */
static int topVisible(void)
{
    int i;

    for (i = g_count - 1; i >= 0; i--) {
        if (!g_wins[i]->minimized)
            return i;
    }
    return -1;
}

/* ---------------- 열기 / 닫기 ---------------- */

struct window *wmOpen(const char *title, const struct app *app, void *data, int w, int h)
{
    struct window *win;
    int n = g_count;

    if (g_count >= WM_MAX)
        return NULL;
    win = (struct window *)calloc(1, sizeof(*win));
    if (win == NULL)
        return NULL;
    strncpy(win->title, title, sizeof(win->title) - 1);
    win->app = app;
    win->data = data;
    win->w = w;
    win->h = h;
    clampWindow(win);
    /* 가운데에서 창마다 조금씩 비켜 놓는다 */
    win->x = (screenCols - win->w) / 2 + (n % 5) * 3 - 6;
    win->y = (areaRows() - win->h) / 2 + (n % 5) - 2;
    clampWindow(win);

    g_wins[g_count++] = win;
    g_active = g_count - 1;
    return win;
}

static void closeAt(int i)
{
    struct window *w = g_wins[i];
    int wasActive = i == g_active;

    if (wasActive)
        g_mode = MODE_NONE;
    if (w->app->close)
        w->app->close(w);
    free(w);
    memmove(&g_wins[i], &g_wins[i + 1], (size_t)(g_count - i - 1) * sizeof(g_wins[0]));
    g_count--;
    if (wasActive)
        g_active = topVisible();
    else if (g_active > i)
        g_active--;
}

/* wantClose 인 창을 닫는다. 닫은 것이 있으면 1 */
int wmReap(void)
{
    int i;
    int closed = 0;

    for (i = g_count - 1; i >= 0; i--) {
        if (g_wins[i]->wantClose) {
            closeAt(i);
            closed = 1;
        }
    }
    return closed;
}

void wmCloseAll(void)
{
    while (g_count > 0)
        closeAt(g_count - 1);
}

int wmCount(void)
{
    return g_count;
}

struct window *wmActive(void)
{
    return g_active >= 0 ? g_wins[g_active] : NULL;
}

/* i 번 창을 활성으로 (최소화돼 있으면 되살린다) */
void wmSetActive(int i)
{
    if (i < 0 || i >= g_count)
        return;
    if (g_wins[i]->minimized) {
        g_wins[i]->minimized = 0;
        g_active = -1;                  /* 아래에서 바뀐 것으로 친다 */
    }
    if (i != g_active) {
        g_active = i;
        g_mode = MODE_NONE;             /* 모드는 그 창에만 */
    }
}

/* i 번 창을 최소화한다. 활성 창이었으면 맨 위의 다른 보이는 창이 활성이 된다 */
void wmMinimize(int i)
{
    if (i < 0 || i >= g_count || g_wins[i]->minimized)
        return;
    g_wins[i]->minimized = 1;
    if (i == g_active) {
        g_mode = MODE_NONE;
        g_active = topVisible();
    }
}

/* 보이는(최소화되지 않은) 창 수 */
int wmVisibleCount(void)
{
    int i;
    int n = 0;

    for (i = 0; i < g_count; i++)
        n += !g_wins[i]->minimized;
    return n;
}

/* 작업 표시줄에 창 이름, 최소화 여부, 활성 창을 알려 준다 */
void wmSyncTaskbar(void)
{
    const char *names[WM_MAX];
    int minimized[WM_MAX];
    int i;

    for (i = 0; i < g_count; i++) {
        names[i] = g_wins[i]->title;
        minimized[i] = g_wins[i]->minimized;
    }
    taskbarSetWindows(names, minimized, g_count, g_active);
}

/* ---------------- 그리기 ---------------- */

static void drawWindow(struct window *w, int active)
{
    enum role title = active ? ROLE_TITLE_ACTIVE : ROLE_TITLE_INACTIVE;

    if (w->layout == LAYOUT_FULL) {
        /* 전체 화면: 테두리/제목 없이 화면 전부 */
        screenFill(0, 0, screenCols, screenRows, ' ', ROLE_WINDOW);
        if (w->app->draw)
            w->app->draw(w, 0, 0, screenCols, screenRows, active);
        return;
    }
    applyLayout(w);                     /* 화면 크기가 바뀌었을 수도 있다 */
    screenFill(w->x, w->y, w->w, w->h, ' ', ROLE_WINDOW);
    screenBox(w->x, w->y, w->w, w->h, ROLE_BORDER);
    screenFill(w->x + 1, w->y, w->w - 2, 1, ' ', title);
    screenText(w->x + 2, w->y, w->title, title);
    if (active && g_mode != MODE_NONE) {
        const char *hint = g_mode == MODE_MOVE ? "[move: arrows  Tab resize  Enter done]"
                                               : "[resize: arrows  Tab move  Enter done]";
        int hx = w->x + w->w - 2 - (int)strlen(hint);

        if (hx > w->x + 2 + (int)strlen(w->title))
            screenText(hx, w->y, hint, ROLE_SELECTION);
        else
            screenText(w->x + w->w - 8, w->y, g_mode == MODE_MOVE ? "[move]" : "[size]", ROLE_SELECTION);
    } else if (w->layout != LAYOUT_NORMAL) {
        screenText(w->x + w->w - 5, w->y, w->layout == LAYOUT_MAX ? "[^]" : w->layout == LAYOUT_LEFT ? "[<]" : "[>]",
                   title);
    }
    if (w->app->draw)
        w->app->draw(w, w->x + 1, w->y + 1, w->w - 2, w->h - 2, active);
}

void wmDraw(void)
{
    int i;

    for (i = 0; i < g_count; i++) {
        if (i != g_active && !g_wins[i]->minimized)
            drawWindow(g_wins[i], 0);
    }
    if (g_active >= 0)
        drawWindow(g_wins[g_active], 1);
}

/* ---------------- 입력 / 이벤트 ---------------- */

/* 배치에 맞춰 자리와 크기를 정한다 (보통이면 창 영역 안으로만) */
static void applyLayout(struct window *w)
{
    int half = screenCols / 2;

    switch (w->layout) {
    case LAYOUT_MAX:
        w->x = 0; w->y = 0; w->w = screenCols; w->h = areaRows();
        break;
    case LAYOUT_LEFT:
        w->x = 0; w->y = 0; w->w = half; w->h = areaRows();
        break;
    case LAYOUT_RIGHT:
        w->x = half; w->y = 0; w->w = screenCols - half; w->h = areaRows();
        break;
    case LAYOUT_FULL:
        w->x = 0; w->y = 0; w->w = screenCols; w->h = screenRows;
        return;
    default:
        break;
    }
    clampWindow(w);
}

void wmSetLayout(struct window *w, int layout)
{
    if (w == NULL || layout == w->layout)
        return;
    if (w->layout == LAYOUT_NORMAL) {
        w->savedX = w->x;
        w->savedY = w->y;
        w->savedW = w->w;
        w->savedH = w->h;
    }
    w->layout = layout;
    if (layout == LAYOUT_NORMAL) {
        w->x = w->savedX;
        w->y = w->savedY;
        w->w = w->savedW;
        w->h = w->savedH;
    }
    applyLayout(w);
}

void wmToggleMaximize(struct window *w)
{
    if (w != NULL)
        wmSetLayout(w, w->layout == LAYOUT_MAX ? LAYOUT_NORMAL : LAYOUT_MAX);
}

/* 활성 창을 왼쪽(-1)/오른쪽(+1) 절반으로. 반대쪽에 붙어 있었으면 보통으로 (Windows 의 Super+방향키) */
void wmSnap(int side)
{
    struct window *w = wmActive();

    if (w == NULL)
        return;
    if ((side < 0 && w->layout == LAYOUT_RIGHT) || (side > 0 && w->layout == LAYOUT_LEFT))
        wmSetLayout(w, LAYOUT_NORMAL);
    else
        wmSetLayout(w, side < 0 ? LAYOUT_LEFT : LAYOUT_RIGHT);
}

/* Super+Down: 보통이 아니면 보통으로, 보통이면 최소화 */
void wmRestoreOrMinimize(void)
{
    struct window *w = wmActive();

    if (w == NULL)
        return;
    if (w->layout != LAYOUT_NORMAL)
        wmSetLayout(w, LAYOUT_NORMAL);
    else
        wmMinimize(g_active);
}

/* F11: 전체 화면 <-> 그 전의 배치 (최대화/절반이었으면 그리로 돌아간다) */
void wmToggleFullscreen(void)
{
    struct window *w = wmActive();

    if (w == NULL)
        return;
    if (w->layout == LAYOUT_FULL) {
        wmSetLayout(w, w->prevLayout);
    } else {
        w->prevLayout = w->layout;
        wmSetLayout(w, LAYOUT_FULL);
    }
}

/* 활성 창이 전체 화면이면 1 (작업 표시줄을 그리지 않는다) */
int wmFullscreenActive(void)
{
    struct window *w = wmActive();

    return w != NULL && w->layout == LAYOUT_FULL;
}

/* ---------------- 여러 창 ---------------- */

static int g_desktopShown;              /* Super+D 로 숨긴 창들 (다시 누르면 되살린다) */
static int g_hiddenByDesktop[WM_MAX];

void wmMinimizeAll(void)
{
    int i;

    for (i = 0; i < g_count; i++)
        g_wins[i]->minimized = 1;
    g_active = -1;
    g_mode = MODE_NONE;
}

void wmRestoreAll(void)
{
    int i;

    for (i = 0; i < g_count; i++)
        g_wins[i]->minimized = 0;
    g_active = topVisible();
    g_desktopShown = 0;
}

/* 바탕 화면 보기: 보이는 창을 모두 숨기고, 다시 누르면 그 창들만 되살린다 */
void wmShowDesktop(void)
{
    int i;
    int any = 0;

    if (g_desktopShown && wmVisibleCount() == 0) {
        for (i = 0; i < g_count; i++) {
            if (g_hiddenByDesktop[i]) {
                g_wins[i]->minimized = 0;
                any = 1;
            }
        }
        g_desktopShown = 0;
        g_active = topVisible();
        if (any)
            return;
    }
    for (i = 0; i < g_count; i++) {
        g_hiddenByDesktop[i] = !g_wins[i]->minimized;
        g_wins[i]->minimized = 1;
    }
    g_desktopShown = 1;
    g_active = -1;
    g_mode = MODE_NONE;
}

/* 작업 표시줄의 n 번째(0 부터) 창: 활성이면 최소화, 아니면 활성으로 (Windows 의 Super+숫자) */
void wmActivateNth(int n)
{
    if (n < 0 || n >= g_count)
        return;
    if (n == g_active && !g_wins[n]->minimized)
        wmMinimize(n);
    else
        wmSetActive(n);
}

/* 제목이 title 인 창의 번호 (없으면 -1) */
int wmFind(const char *title)
{
    int i;

    for (i = 0; i < g_count; i++) {
        if (strcmp(g_wins[i]->title, title) == 0)
            return i;
    }
    return -1;
}

const char *wmTitle(int i)
{
    return (i >= 0 && i < g_count) ? g_wins[i]->title : "";
}

int wmIsMinimized(int i)
{
    return (i >= 0 && i < g_count) ? g_wins[i]->minimized : 0;
}

int wmActiveIndex(void)
{
    return g_active;
}

/* 옮기기/크기 조절 모드의 키. 모드 중에는 모든 키를 먹는다 */
static void modeKey(struct window *w, int key)
{
    int dx = 0;
    int dy = 0;

    switch (key) {
    case KEY_LEFT:  dx = -1; break;
    case KEY_RIGHT: dx = 1; break;
    case KEY_UP:    dy = -1; break;
    case KEY_DOWN:  dy = 1; break;
    case KEY_PGUP:  dy = -5; break;
    case KEY_PGDN:  dy = 5; break;
    case KEY_HOME:  dx = -screenCols; break;
    case KEY_END:   dx = screenCols; break;
    case '\t':
        g_mode = g_mode == MODE_MOVE ? MODE_RESIZE : MODE_MOVE;
        return;
    case '\r':
    case '\n':
    case KEY_ESC:
        g_mode = MODE_NONE;
        return;
    default:
        return;
    }
    if (g_mode == MODE_MOVE) {
        w->x += dx;
        w->y += dy;
    } else {
        w->w += dx;
        w->h += dy;
    }
    clampWindow(w);
}

/* 처리했으면 1 */
int wmKey(int key)
{
    struct window *w = wmActive();

    if (w == NULL)
        return 0;
    if (g_mode != MODE_NONE) {
        modeKey(w, KEY_BASE(key));
        return 1;
    }
    if (wmMenuKey(key))
        return 1;
    return w->app->key ? w->app->key(w, key) : 0;
}

void wmCloseActive(void)
{
    struct window *w = wmActive();

    if (w != NULL)
        w->wantClose = 1;
}

void wmMinimizeActive(void)
{
    wmMinimize(g_active);
}

/* 활성 창의 옮기기(resize = 0)/크기 조절(1) 모드 (보통이 아닌 배치는 먼저 되돌린다) */
void wmStartMove(int resize)
{
    struct window *w = wmActive();

    if (w == NULL)
        return;
    wmSetLayout(w, LAYOUT_NORMAL);
    g_mode = resize ? MODE_RESIZE : MODE_MOVE;
}

/* ---------------- 창 메뉴 (Alt+Space) ---------------- */

static const struct {
    char key;
    const char *label;
} g_wmenu[] = {
    { 'r', "Restore" }, { 'm', "Move" }, { 's', "Size" }, { 'n', "Minimize" },
    { 'x', "Maximize" }, { 'f', "Full screen" }, { 'c', "Close" },
};
#define WMENU_COUNT ((int)(sizeof(g_wmenu) / sizeof(g_wmenu[0])))

static int g_wmenuOpen;
static int g_wmenuSel;

void wmMenuOpen(void)
{
    if (wmActive() == NULL)
        return;
    g_wmenuOpen = 1;
    g_wmenuSel = 0;
}

int wmMenuIsOpen(void)
{
    return g_wmenuOpen;
}

void wmMenuDraw(void)
{
    struct window *w = wmActive();
    int x;
    int y;
    int i;
    char line[32];

    if (!g_wmenuOpen || w == NULL)
        return;
    x = w->layout == LAYOUT_FULL ? 0 : w->x + 1;
    y = w->layout == LAYOUT_FULL ? 0 : w->y + 1;
    if (x + 20 > screenCols)
        x = screenCols - 20;
    screenFill(x, y, 20, WMENU_COUNT + 2, ' ', ROLE_WINDOW);
    screenBox(x, y, 20, WMENU_COUNT + 2, ROLE_BORDER);
    for (i = 0; i < WMENU_COUNT; i++) {
        enum role r = i == g_wmenuSel ? ROLE_SELECTION : ROLE_WINDOW;

        snprintf(line, sizeof(line), " [%c] %-12s", g_wmenu[i].key, g_wmenu[i].label);
        screenText(x + 1, y + 1 + i, line, r);
    }
}

static void wmenuDo(char k)
{
    struct window *w = wmActive();

    g_wmenuOpen = 0;
    if (w == NULL)
        return;
    switch (k) {
    case 'r': wmSetLayout(w, LAYOUT_NORMAL); break;
    case 'm': wmStartMove(0); break;
    case 's': wmStartMove(1); break;
    case 'n': wmMinimize(g_active); break;
    case 'x': wmSetLayout(w, LAYOUT_MAX); break;
    case 'f': wmToggleFullscreen(); break;
    case 'c': w->wantClose = 1; break;
    default: break;
    }
}

/* 열려 있으면 모든 키를 먹는다 */
int wmMenuKey(int key)
{
    int i;
    int k = KEY_BASE(key);

    if (!g_wmenuOpen)
        return 0;
    if (wmActive() == NULL) {
        g_wmenuOpen = 0;
        return 0;
    }
    switch (k) {
    case KEY_ESC:
        g_wmenuOpen = 0;
        return 1;
    case KEY_UP:
        g_wmenuSel = (g_wmenuSel + WMENU_COUNT - 1) % WMENU_COUNT;
        return 1;
    case KEY_DOWN:
        g_wmenuSel = (g_wmenuSel + 1) % WMENU_COUNT;
        return 1;
    case '\r':
        wmenuDo(g_wmenu[g_wmenuSel].key);
        return 1;
    default:
        break;
    }
    for (i = 0; i < WMENU_COUNT; i++) {
        if (g_wmenu[i].key == k || g_wmenu[i].key + ('A' - 'a') == k) {
            wmenuDo(g_wmenu[i].key);
            return 1;
        }
    }
    return 1;
}

/*
 * 모든 창이 기다리는 fd 를 fds[] 에 모은다. owner[k] 는 fds[k] 의 창 번호. 모은 개수를 돌려준다.
 * 창이 바뀌기 전에 wmPollDone 으로 결과를 넘겨야 한다 (같은 루프 안).
 */
int wmPollFds(struct pollfd *fds, int *owner, int max)
{
    int i;
    int n = 0;

    for (i = 0; i < g_count && n < max; i++) {
        struct window *w = g_wins[i];
        int k;
        int got;

        if (w->app->pollFds == NULL)
            continue;
        got = w->app->pollFds(w, fds + n, max - n);
        for (k = 0; k < got; k++)
            owner[n + k] = i;
        n += got;
    }
    return n;
}

/* poll 결과를 창에 넘긴다. 무언가 바뀌었으면 1 (다시 그린다) */
int wmPollDone(const struct pollfd *fds, const int *owner, int n)
{
    int k;
    int dirty = 0;

    for (k = 0; k < n; k++) {
        struct window *w = g_wins[owner[k]];

        if (fds[k].revents && w->app->ready) {
            w->app->ready(w, &fds[k]);
            dirty = 1;
        }
    }
    return dirty;
}

/* 이벤트 루프가 깰 때마다 (시간 초과 포함). 무언가 바뀌었으면 1 */
int wmTick(void)
{
    int i;
    int dirty = 0;

    for (i = 0; i < g_count; i++) {
        if (g_wins[i]->app->tick && g_wins[i]->app->tick(g_wins[i]))
            dirty = 1;
    }
    return dirty;
}
