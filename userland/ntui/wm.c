/*
 * wm.c - 창 관리: 창 목록, 테두리/제목줄, 포커스, 최대화, 앱에 키와 이벤트 넘기기
 *
 * 창 영역은 화면에서 작업 표시줄(맨 아래 TASKBAR_ROWS 줄)을 뺀 곳이다.
 * 창 목록의 순서는 만든 순서 그대로다 (작업 표시줄의 순서). 겹칠 때는 활성 창을 맨 위에 그린다.
 * 앱(struct app)은 창 안쪽(테두리 안)만 그린다. 앱이 창을 닫고 싶으면 w->wantClose = 1 로 두면
 * 다음 wmReap 이 닫는다.
 *
 * 창 단축키 (wmKey): Ctrl-W 닫기, Ctrl-F 최대화/되돌리기, Ctrl-O 옮기기/크기 조절 모드.
 * 그 밖의 키는 활성 창의 앱이 받는다.
 *
 * 옮기기 모드 (Ctrl-O): 방향키로 활성 창을 한 칸씩 옮긴다 (PgUp/PgDn 은 위/아래로 크게, Home/End 는 왼쪽/오른쪽 끝).
 *   Tab 을 누르면 크기 조절 모드: -> / <- 로 넓게/좁게, ↓ / ↑ 로 높게/낮게. 다시 Tab 이면 옮기기.
 *   Enter, Esc, Ctrl-O 로 끝낸다. 모드 중에는 키가 앱으로 가지 않는다. 최대화된 창은 먼저 되돌린다.
 */
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

    if (i == g_active)
        g_mode = MODE_NONE;
    if (w->app->close)
        w->app->close(w);
    free(w);
    memmove(&g_wins[i], &g_wins[i + 1], (size_t)(g_count - i - 1) * sizeof(g_wins[0]));
    g_count--;
    if (g_active >= g_count)
        g_active = g_count - 1;
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

void wmSetActive(int i)
{
    if (i >= 0 && i < g_count && i != g_active) {
        g_active = i;
        g_mode = MODE_NONE;             /* 모드는 그 창에만 */
    }
}

/* 작업 표시줄에 창 이름과 활성 창을 알려 준다 */
void wmSyncTaskbar(void)
{
    const char *names[WM_MAX];
    int i;

    for (i = 0; i < g_count; i++)
        names[i] = g_wins[i]->title;
    taskbarSetWindows(names, g_count, g_active);
}

/* ---------------- 그리기 ---------------- */

static void drawWindow(struct window *w, int active)
{
    enum role title = active ? ROLE_TITLE_ACTIVE : ROLE_TITLE_INACTIVE;

    clampWindow(w);                     /* 화면 크기가 바뀌었을 수도 있다 */
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
    } else if (w->maximized) {
        screenText(w->x + w->w - 5, w->y, "[^]", title);
    }
    if (w->app->draw)
        w->app->draw(w, w->x + 1, w->y + 1, w->w - 2, w->h - 2, active);
}

void wmDraw(void)
{
    int i;

    for (i = 0; i < g_count; i++) {
        if (i != g_active)
            drawWindow(g_wins[i], 0);
    }
    if (g_active >= 0)
        drawWindow(g_wins[g_active], 1);
}

/* ---------------- 입력 / 이벤트 ---------------- */

static void toggleMaximize(struct window *w)
{
    if (!w->maximized) {
        w->savedX = w->x;
        w->savedY = w->y;
        w->savedW = w->w;
        w->savedH = w->h;
        w->x = 0;
        w->y = 0;
        w->w = screenCols;
        w->h = areaRows();
        w->maximized = 1;
    } else {
        w->x = w->savedX;
        w->y = w->savedY;
        w->w = w->savedW;
        w->h = w->savedH;
        w->maximized = 0;
        clampWindow(w);
    }
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
    case KEY_CTRL('o'):
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
        modeKey(w, key);
        return 1;
    }
    if (key == KEY_CTRL('o')) {
        if (w->maximized)
            toggleMaximize(w);
        g_mode = MODE_MOVE;
        return 1;
    }
    if (key == KEY_CTRL('w')) {
        w->wantClose = 1;
        return 1;
    }
    if (key == KEY_CTRL('f')) {
        toggleMaximize(w);
        return 1;
    }
    return w->app->key ? w->app->key(w, key) : 0;
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
