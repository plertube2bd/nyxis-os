/*
 * nclock - 시계 / 달력 (NWP 창 프로그램). q / Esc 끝. 원래 설명: 시계 / 달력 창 (작업 표시줄의 시계 위젯: 작업 표시줄(Super+B)에서 시계를 고르고 Enter, 또는 메뉴의 Clock & calendar)
 *
 *    14:32:07
 *    Wednesday, 11 November 2026
 *    Europe/London  UTC+00:00
 *    up 1h 02m
 *
 *         November 2026
 *    Mo Tu We Th Fr Sa Su
 *                       1        (오늘은 강조)
 *
 * <- / -> 이전 / 다음 달, t 오늘. 초가 바뀔 때마다 다시 그린다 (이벤트 루프는 1초마다 깬다).
 * 업타임은 nx64_info_ulong("uptime_ms"). 시각은 time() + utcOffset(분). nlibc 의 localtime 은 UTC 뿐이라 날짜는 직접 계산한다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <nx64.h>

#define NWP_SHORT_NAMES
#include <nwp.h>

/* 창 (NWP) */
static struct nwp_window *g_w;
struct window {
    void *data;
};
#define screenText(x, y, s, r)          nwp_text(g_w, (x), (y), (s), (r))
#define screenFill(x, y, cw, chh, c, r) nwp_fill(g_w, (x), (y), (cw), (chh), (c), (r))

static int utcOffset;                   /* 분 (info:/utc_offset "+HH:MM") */
static char timezoneName[64];           /* info:/timezone */

static void loadZone(void)
{
    char buf[16];

    if (nx64_info("timezone", timezoneName, sizeof(timezoneName)) <= 0)
        strcpy(timezoneName, "UTC");
    utcOffset = 0;
    if (nx64_info("utc_offset", buf, sizeof(buf)) == 6 && (buf[0] == '+' || buf[0] == '-') && buf[3] == ':') {
        int m = ((buf[1] - '0') * 10 + (buf[2] - '0')) * 60 + (buf[4] - '0') * 10 + (buf[5] - '0');

        utcOffset = buf[0] == '-' ? -m : m;
    }
}

struct clock {
    long shown;                         /* 마지막으로 그린 시각 (초) */
    int year;                           /* 달력에 보이는 달 */
    int month;                          /* 1..12 */
};

static const char *const g_months[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
static const char *const g_days[7] = {
    "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"
};

/* 1970-01-01 부터의 날 수 -> 연/월/일 (그레고리력, 음수도 된다) */
static void civil(long days, int *y, int *m, int *d)
{
    long z = days + 719468L;
    long era = (z >= 0 ? z : z - 146096L) / 146097L;
    long doe = z - era * 146097L;
    long yoe = (doe - doe / 1460L + doe / 36524L - doe / 146096L) / 365L;
    long doy = doe - (365L * yoe + yoe / 4L - yoe / 100L);
    long mp = (5L * doy + 2L) / 153L;

    *d = (int)(doy - (153L * mp + 2L) / 5L + 1L);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400L + (*m <= 2 ? 1 : 0));
}

/* 연/월/일 -> 1970-01-01 부터의 날 수 */
static long days_from_civil(int y, int m, int d)
{
    long yy = (long)y - (m <= 2 ? 1 : 0);
    long era = (yy >= 0 ? yy : yy - 399L) / 400L;
    long yoe = yy - era * 400L;
    long doy = (153L * (m > 2 ? m - 3 : m + 9) + 2L) / 5L + d - 1L;
    long doe = yoe * 365L + yoe / 4L - yoe / 100L + doy;

    return era * 146097L + doe - 719468L;
}

static int days_in_month(int y, int m)
{
    static const int n[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return n[m - 1];
}

/* 월요일 = 0 */
static int weekday(long days)
{
    long w = (days + 3) % 7;            /* 1970-01-01 은 목요일 */

    return (int)(w < 0 ? w + 7 : w);
}

static long local_seconds(void)
{
    time_t now = time(NULL);

    if (now == (time_t)-1)
        return -1;
    return (long)now + (long)utcOffset * 60L;
}

static void clock_today(struct clock *c)
{
    long s = local_seconds();
    int d;

    if (s < 0) {
        c->year = 1970;
        c->month = 1;
        return;
    }
    civil(s / 86400L - (s % 86400L < 0 ? 1 : 0), &c->year, &c->month, &d);
}

static void clockDraw(struct window *w, int x, int y, int cols, int rows, int active)
{
    struct clock *c = (struct clock *)w->data;
    long s = local_seconds();
    char line[64];
    unsigned long up;
    int ty = 0, tm = 0, td = 0;
    long today = 0;
    long first;
    int start;
    int n;
    int i;
    int row;
    int r = ROLE_WINDOW;

    (void)active;
    screenFill(x, y, cols, rows, ' ', r);
    if (s >= 0) {
        long sod = s % 86400L;

        if (sod < 0)
            sod += 86400L;
        today = s / 86400L - (s % 86400L < 0 ? 1 : 0);
        civil(today, &ty, &tm, &td);
        snprintf(line, sizeof(line), "%02ld:%02ld:%02ld", sod / 3600L, sod / 60L % 60L, sod % 60L);
        screenText(x + 2, y + 1, line, ROLE_TITLE_ACTIVE);
        snprintf(line, sizeof(line), "%s, %d %s %d", g_days[weekday(today)], td, g_months[tm - 1], ty);
        screenText(x + 2, y + 2, line, r);
        c->shown = s;
    } else {
        screenText(x + 2, y + 1, "--:--:--", ROLE_TITLE_ACTIVE);
    }
    snprintf(line, sizeof(line), "%s  UTC%c%02d:%02d", timezoneName, utcOffset < 0 ? '-' : '+',
             abs(utcOffset) / 60, abs(utcOffset) % 60);
    screenText(x + 2, y + 3, line, ROLE_BORDER);
    if (nx64_info_ulong("uptime_ms", &up) == 0) {
        long u = (long)(up / 1000UL);

        snprintf(line, sizeof(line), "up %ldd %ldh %02ldm", u / 86400L, u / 3600L % 24L, u / 60L % 60L);
        screenText(x + 2, y + 4, line, ROLE_BORDER);
    }

    /* 달력 */
    snprintf(line, sizeof(line), "%s %d", g_months[c->month - 1], c->year);
    screenText(x + (cols - (int)strlen(line)) / 2, y + 6, line, r);
    screenText(x + 2, y + 7, "Mo Tu We Th Fr Sa Su", ROLE_BORDER);
    first = days_from_civil(c->year, c->month, 1);
    start = weekday(first);
    n = days_in_month(c->year, c->month);
    row = 0;
    for (i = 1; i <= n; i++) {
        int col = (start + i - 1) % 7;
        int cy = y + 8 + (start + i - 1) / 7;

        row = (start + i - 1) / 7;
        snprintf(line, sizeof(line), "%2d", i);
        screenText(x + 2 + col * 3, cy, line,
                   (s >= 0 && c->year == ty && c->month == tm && i == td) ? ROLE_SELECTION : r);
    }
    if (y + 8 + row + 2 < y + rows)
        screenText(x + 2, y + rows - 1, "<- -> month   t today", ROLE_BORDER);
}

static int clockKey(struct window *w, int key)
{
    struct clock *c = (struct clock *)w->data;

    switch (key) {
    case KEY_LEFT:
        if (--c->month < 1) {
            c->month = 12;
            c->year--;
        }
        return 1;
    case KEY_RIGHT:
        if (++c->month > 12) {
            c->month = 1;
            c->year++;
        }
        return 1;
    case 't':
        clock_today(c);
        return 1;
    }
    return 0;
}

int main(void)
{
    static struct clock c;
    struct window win;

    loadZone();
    c.shown = -1;
    clock_today(&c);
    win.data = &c;
    g_w = nwp_open("nclock", "clock", 28, 16, NWP_STATE_NORMAL);
    if (g_w == NULL) {
        fprintf(stderr, "nclock: cannot open a window (run it inside ntui)\n");
        return 1;
    }
    for (;;) {
        struct nwp_event ev;
        int r;

        clockDraw(&win, 0, 0, nwp_cols(g_w), nwp_rows(g_w), nwp_focused(g_w));
        (void)nwp_commit(g_w);
        r = nwp_next_event(g_w, &ev, 1000);    /* 1초마다 시계를 다시 그린다 */
        if (r < 0 || (r > 0 && ev.type == NWP_EVENT_CLOSE))
            break;
        if (r > 0 && ev.type == NWP_EVENT_KEY) {
            if (ev.key == 'q' || ev.key == NWP_KEY_ESC)
                break;
            (void)clockKey(&win, ev.key);
        }
    }
    nwp_close(g_w);
    return 0;
}
