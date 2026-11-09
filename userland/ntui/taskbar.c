/*
 * taskbar.c - 작업 표시줄 (화면 맨 아래 한 줄)
 *
 *   [ntui]  ntui | files | about                  test@nyxis  Europe/London  12:34
 *
 * - 왼쪽: 메뉴 버튼 "[ntui]" (Ctrl-Q 로 연다: menu.c. 열려 있으면 강조)
 * - 가운데: 창 목록. 창 관리(wm)가 생기면 wm 이 taskbarSetWindows 로 알려 준다 (작업 표시줄은 창 구조체를
 *   모른다). 활성 창은 ROLE_SELECTION. 자리가 모자라면 "..." 로 줄인다.
 * - 오른쪽: username@hostname, 시간대, 시계. 좁으면 시간대 -> 사용자 순으로 뺀다 (시계는 늘 남긴다).
 * - 시계: time() + utcOffset(분). nlibc 의 localtime 은 UTC 뿐이라 직접 계산한다. 분이 바뀔 때만 다시 그린다.
 * - 입력: Ctrl-N / Ctrl-P 로 다음/이전 창 (언제나). Ctrl-B 로 작업 표시줄 포커스를 켜면 <- / -> 로 고르고
 *   Enter 로 바꾸고, ESC 로 나온다. 포커스 중에 고르는 항목은 ROLE_TITLE_ACTIVE 로 보인다.
 * 색은 ROLE_TASKBAR / ROLE_SELECTION / ROLE_TITLE_ACTIVE 만 쓴다 (Mono 에서는 반전으로 구분된다).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ntui.h"

#define TASKBAR_ITEMS_MAX 16
#define TASKBAR_NAME_MAX  24

static char g_names[TASKBAR_ITEMS_MAX][TASKBAR_NAME_MAX];
static int g_count;
static int g_active = -1;               /* 활성 창 (-1 = 없음) */
static int g_focus;                     /* 1 이면 작업 표시줄이 키를 받는다 */
static int g_cursor;                    /* 포커스 중에 고르고 있는 항목 */
static long g_shownMinute = -1;         /* 마지막으로 그린 시계 (분 단위 시각) */

/* ---------------- 창 목록 ---------------- */

void taskbarSetWindows(const char *const *names, int count, int active)
{
    int i;

    if (count > TASKBAR_ITEMS_MAX)
        count = TASKBAR_ITEMS_MAX;
    if (count < 0)
        count = 0;
    for (i = 0; i < count; i++) {
        strncpy(g_names[i], names[i], TASKBAR_NAME_MAX - 1);
        g_names[i][TASKBAR_NAME_MAX - 1] = '\0';
    }
    g_count = count;
    g_active = (active >= 0 && active < count) ? active : (count > 0 ? 0 : -1);
    if (g_cursor >= count)
        g_cursor = g_active < 0 ? 0 : g_active;
}

int taskbarActive(void)
{
    return g_active;
}

int taskbarFocused(void)
{
    return g_focus;
}

/* ---------------- 시계 ---------------- */

/* 지역 시각(분 단위, 하루 안의 분). 시각을 모르면 -1 */
static long localMinuteOfDay(long *outAbsMinute)
{
    time_t now = time(NULL);
    long m;

    if (now == (time_t)-1)
        return -1;
    m = (long)(now / 60) + utcOffset;   /* 1970 년 이후 분 */
    if (outAbsMinute)
        *outAbsMinute = m;
    m %= 24L * 60L;
    if (m < 0)
        m += 24L * 60L;
    return m;
}

/* 시계를 다시 그려야 하면 1 (분이 바뀌었다). 이벤트 루프가 키 대기 시간 초과마다 부른다 */
int taskbarTick(void)
{
    long abs = -1;

    (void)localMinuteOfDay(&abs);
    return abs != g_shownMinute;
}

/* ---------------- 그리기 ---------------- */

static int textLen(const char *s)
{
    return (int)strlen(s);
}

/* 가운데 창 목록을 [x, end) 안에 그린다 */
static void drawWindows(int x, int end, int y)
{
    int i;

    for (i = 0; i < g_count; i++) {
        int need = textLen(g_names[i]) + 2;         /* 앞뒤 빈칸 */
        enum role r = ROLE_TASKBAR;

        if (i > 0) {
            if (x + 1 > end)
                break;
            screenPut(x++, y, '|', ROLE_TASKBAR);
        }
        /* 이 항목이 안 들어가면 "..." 로 끝낸다 (그 자리도 없으면 그냥 끝) */
        if (x + need > end) {
            if (x + 4 <= end)
                screenText(x, y, " ...", ROLE_TASKBAR);
            break;
        }
        if (g_focus && i == g_cursor)
            r = ROLE_TITLE_ACTIVE;
        else if (i == g_active)
            r = ROLE_SELECTION;
        screenPut(x, y, ' ', r);
        screenText(x + 1, y, g_names[i], r);
        screenPut(x + need - 1, y, ' ', r);
        x += need;
    }
}

void taskbarDraw(void)
{
    int y = screenRows - 1;
    char clock[8];
    char user[64 + 64 + 2];
    long abs = -1;
    long mod = localMinuteOfDay(&abs);
    int right;
    int left;
    int showUser;
    int showZone;

    if (y < 0)
        return;
    screenFill(0, y, screenCols, 1, ' ', ROLE_TASKBAR);

    /* 오른쪽부터 자리를 정한다: 시계 > 사용자 > 시간대 */
    if (mod < 0)
        strcpy(clock, "--:--");
    else
        snprintf(clock, sizeof(clock), "%02ld:%02ld", mod / 60, mod % 60);
    snprintf(user, sizeof(user), "%s@%s", username, hostname);
    g_shownMinute = abs;

    left = 7;                                       /* "[ntui] " */
    right = textLen(clock) + 1;
    showUser = left + 10 + right + textLen(user) + 2 <= screenCols;
    if (showUser)
        right += textLen(user) + 2;
    showZone = showUser && left + 10 + right + textLen(timezoneName) + 2 <= screenCols;
    if (showZone)
        right += textLen(timezoneName) + 2;

    /* 그리기 */
    screenText(0, y, "[ntui]", menuIsOpen() ? ROLE_SELECTION : ROLE_TASKBAR);   /* 메뉴 (Ctrl-Q) */
    {
        int x = screenCols - right;

        if (showUser) {
            screenText(x, y, user, ROLE_TASKBAR);
            x += textLen(user) + 2;
        }
        if (showZone) {
            screenText(x, y, timezoneName, ROLE_TASKBAR);
            x += textLen(timezoneName) + 2;
        }
        screenText(x, y, clock, ROLE_TASKBAR);
    }
    drawWindows(left, screenCols - right - 1, y);
}

/* ---------------- 입력 ---------------- */

static void cycle(int step)
{
    if (g_count == 0)
        return;
    g_active = ((g_active < 0 ? 0 : g_active) + step + g_count) % g_count;
    g_cursor = g_active;
}

/* 처리했으면 1 (호출자는 다시 그린다), 아니면 0 (다른 곳에 넘긴다) */
int taskbarKey(int key)
{
    if (key == KEY_CTRL('n')) {
        cycle(1);
        return 1;
    }
    if (key == KEY_CTRL('p')) {
        cycle(-1);
        return 1;
    }
    if (key == KEY_CTRL('b')) {
        g_focus = !g_focus;
        g_cursor = g_active < 0 ? 0 : g_active;
        return 1;
    }
    if (!g_focus)
        return 0;

    switch (key) {
    case KEY_LEFT:
        if (g_count > 0)
            g_cursor = (g_cursor + g_count - 1) % g_count;
        return 1;
    case KEY_RIGHT:
        if (g_count > 0)
            g_cursor = (g_cursor + 1) % g_count;
        return 1;
    case '\r':
        if (g_count > 0)
            g_active = g_cursor;
        g_focus = 0;
        return 1;
    case KEY_ESC:
        g_focus = 0;
        return 1;
    }
    return 1;                           /* 포커스 중에는 다른 키를 창으로 넘기지 않는다 */
}
