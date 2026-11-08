/*
 * time.c - C89 7.12 (시간대는 UTC 하나: time.h 머리말)
 *
 * 날짜 계산은 그레고리력 일 번호 변환(1970-01-01 = 0일)으로 한다. 윤초는 없다.
 */
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "nl_internal.h"

clock_t clock(void)
{
    long now = __nl_clock_ns(NL_CLOCK_MONOTONIC);

    if (now < 0 || __nl_start_ns < 0)
        return (clock_t)-1;
    return (clock_t)((now - __nl_start_ns) / (1000000000L / CLOCKS_PER_SEC));
}

time_t time(time_t *timer)
{
    long ns = __nl_clock_ns(NL_CLOCK_REALTIME);
    time_t t = ns < 0 ? (time_t)-1 : (time_t)(ns / 1000000000L);

    if (timer)
        *timer = t;
    return t;
}

double difftime(time_t time1, time_t time0)
{
    return (double)time1 - (double)time0;
}

/* 내림 나눗셈 (음수에서도 바닥으로) */
static long fdiv(long a, long b)
{
    return a / b - ((a % b != 0) && ((a < 0) != (b < 0)));
}

/* 그레고리력 (y, m=1..12, d) -> 1970-01-01 이후 일 수 */
static long days_from_civil(long y, long m, long d)
{
    long era;
    long yoe;
    long doy;
    long doe;

    y -= m <= 2;
    era = fdiv(y, 400);
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* t -> tm (UTC). 연도가 int 에 들어가지 않으면 -1 */
static int to_tm(time_t t, struct tm *tm)
{
    long days = fdiv(t, 86400);
    long secs = t - days * 86400;
    long z = days + 719468;
    long era = fdiv(z, 146097);
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1;
    long m = mp < 10 ? mp + 3 : mp - 9;

    y += m <= 2;
    if (y - 1900 > INT_MAX || y - 1900 < INT_MIN)
        return -1;
    tm->tm_year = (int)(y - 1900);
    tm->tm_mon = (int)(m - 1);
    tm->tm_mday = (int)d;
    tm->tm_hour = (int)(secs / 3600);
    tm->tm_min = (int)(secs / 60 % 60);
    tm->tm_sec = (int)(secs % 60);
    tm->tm_wday = (int)((days % 7 + 11) % 7);           /* 1970-01-01 은 목요일(4) */
    tm->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    tm->tm_isdst = 0;
    return 0;
}

struct tm *gmtime(const time_t *timer)
{
    static struct tm tm;

    if (to_tm(*timer, &tm) < 0) {
        errno = ERANGE;
        return NULL;
    }
    return &tm;
}

struct tm *localtime(const time_t *timer)
{
    return gmtime(timer);
}

/* 각 필드를 범위 밖 값까지 허용해 정규화한다 (C89 7.12.2.3). tm_wday/tm_yday 는 무시하고 다시 계산 */
time_t mktime(struct tm *timeptr)
{
    long y = (long)timeptr->tm_year + 1900L;
    long mon = timeptr->tm_mon;
    long days;
    long t;
    const long lim = 1L << 40;          /* 곱셈이 넘치지 않을 만큼 넉넉한 상한 */

    y += fdiv(mon, 12);
    mon -= fdiv(mon, 12) * 12;
    if (y > lim || y < -lim)
        return (time_t)-1;
    days = days_from_civil(y, mon + 1, 1) + (long)timeptr->tm_mday - 1;
    t = days * 86400L + (long)timeptr->tm_hour * 3600L + (long)timeptr->tm_min * 60L + (long)timeptr->tm_sec;
    if (to_tm(t, timeptr) < 0)
        return (time_t)-1;
    return t;
}

static const char *const wday_name[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char *const mon_name[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static const char *wday_of(const struct tm *t)
{
    return (t->tm_wday >= 0 && t->tm_wday < 7) ? wday_name[t->tm_wday] : "???";
}

static const char *mon_of(const struct tm *t)
{
    return (t->tm_mon >= 0 && t->tm_mon < 12) ? mon_name[t->tm_mon] : "???";
}

/* C89 7.12.3.1 이 정한 형식 그대로 */
char *asctime(const struct tm *timeptr)
{
    static char buf[64];

    sprintf(buf, "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n",
            wday_of(timeptr), mon_of(timeptr), timeptr->tm_mday,
            timeptr->tm_hour, timeptr->tm_min, timeptr->tm_sec, 1900 + timeptr->tm_year);
    return buf;
}

char *ctime(const time_t *timer)
{
    struct tm *t = localtime(timer);

    return t ? asctime(t) : NULL;
}

struct out {
    char *s;
    size_t max;
    size_t n;
    int overflow;
};

static void put(struct out *o, const char *str)
{
    while (*str) {
        if (o->n + 1 >= o->max) {
            o->overflow = 1;
            return;
        }
        o->s[o->n++] = *str++;
    }
}

static void put_num(struct out *o, long v, int width, char pad)
{
    char buf[24];
    char fmt[8];

    if (pad == '0')
        sprintf(fmt, "%%0%dld", width);
    else
        sprintf(fmt, "%%%dld", width);
    sprintf(buf, fmt, v);
    put(o, buf);
}

static void format(struct out *o, const char *f, const struct tm *t)
{
    for (; *f && !o->overflow; f++) {
        char one[2];

        if (*f != '%' || f[1] == '\0') {
            one[0] = *f;
            one[1] = '\0';
            put(o, one);
            continue;
        }
        switch (*++f) {
        case 'a': { char b[4]; memcpy(b, wday_of(t), 3); b[3] = '\0'; put(o, b); break; }
        case 'A': put(o, wday_of(t)); break;
        case 'b': { char b[4]; memcpy(b, mon_of(t), 3); b[3] = '\0'; put(o, b); break; }
        case 'B': put(o, mon_of(t)); break;
        case 'c': format(o, "%a %b %e %H:%M:%S %Y", t); break;
        case 'd': put_num(o, t->tm_mday, 2, '0'); break;
        case 'e': put_num(o, t->tm_mday, 2, ' '); break;     /* C99, %c 에서 쓴다 */
        case 'H': put_num(o, t->tm_hour, 2, '0'); break;
        case 'I': put_num(o, t->tm_hour % 12 == 0 ? 12 : t->tm_hour % 12, 2, '0'); break;
        case 'j': put_num(o, t->tm_yday + 1, 3, '0'); break;
        case 'm': put_num(o, t->tm_mon + 1, 2, '0'); break;
        case 'M': put_num(o, t->tm_min, 2, '0'); break;
        case 'p': put(o, t->tm_hour < 12 ? "AM" : "PM"); break;
        case 'S': put_num(o, t->tm_sec, 2, '0'); break;
        case 'U': put_num(o, (t->tm_yday + 7 - t->tm_wday) / 7, 2, '0'); break;
        case 'w': put_num(o, t->tm_wday, 1, '0'); break;
        case 'W': put_num(o, (t->tm_yday + 7 - (t->tm_wday + 6) % 7) / 7, 2, '0'); break;
        case 'x': format(o, "%m/%d/%y", t); break;
        case 'X': format(o, "%H:%M:%S", t); break;
        case 'y': put_num(o, ((long)t->tm_year + 1900L) % 100 < 0 ? -(((long)t->tm_year + 1900L) % 100)
                                                                 : ((long)t->tm_year + 1900L) % 100, 2, '0'); break;
        case 'Y': put_num(o, (long)t->tm_year + 1900L, 1, '0'); break;
        case 'Z': put(o, "UTC"); break;
        case '%': put(o, "%"); break;
        default:                                            /* 모르는 변환: 그대로 */
            one[0] = '%';
            one[1] = '\0';
            put(o, one);
            one[0] = *f;
            put(o, one);
            break;
        }
    }
}

size_t strftime(char *s, size_t maxsize, const char *fmt, const struct tm *timeptr)
{
    struct out o;

    if (maxsize == 0)
        return 0;
    o.s = s;
    o.max = maxsize;
    o.n = 0;
    o.overflow = 0;
    format(&o, fmt, timeptr);
    if (o.overflow)
        return 0;
    s[o.n] = '\0';
    return o.n;
}

/* ---------------- POSIX ---------------- */
int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    long ns;

    if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) {
        errno = EINVAL;
        return -1;
    }
    ns = __nl_clock_ns(clock_id == CLOCK_REALTIME ? NL_CLOCK_REALTIME : NL_CLOCK_MONOTONIC);
    if (ns < 0)
        return -1;
    tp->tv_sec = ns / 1000000000L;
    tp->tv_nsec = ns % 1000000000L;
    return 0;
}

/* 커널의 잠들기 단위는 밀리초다: 올려서 잔다 (요청보다 짧게 자지 않는다). 중단되는 일이 없어 rem 은 0 */
int nanosleep(const struct timespec *req, struct timespec *rem)
{
    unsigned long ms;

    if (req == NULL || req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    ms = (unsigned long)req->tv_sec * 1000UL + ((unsigned long)req->tv_nsec + 999999UL) / 1000000UL;
    if (__nl_sleep_ms(ms) < 0)
        return -1;
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

/* ---------------- POSIX: gettimeofday ---------------- */
#include <sys/time.h>

int gettimeofday(struct timeval *tv, void *tz)
{
    struct timespec ts;

    (void)tz;
    if (tv == NULL)
        return 0;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return -1;
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000L;
    return 0;
}
