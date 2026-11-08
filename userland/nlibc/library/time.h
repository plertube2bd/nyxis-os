/*
 * time.h - C89 7.12
 *
 * time_t = 1970-01-01 00:00:00 UTC 이후 초. 시간대는 UTC 하나뿐이다 (localtime == gmtime).
 * clock() 은 프로세스별 CPU 시간이 없어서 프로그램 시작 후 경과한 실제 시간으로 근사한다.
 */
#ifndef _NL_TIME_H
#define _NL_TIME_H

#define _NL_WANT_SIZE_T
#define _NL_WANT_NULL
#define _NL_WANT_TIME_T
#include "_nl_types.h"

#define CLOCKS_PER_SEC 1000000L

typedef long clock_t;

struct tm {
    int tm_sec;     /* 0~60 (윤초) */
    int tm_min;     /* 0~59 */
    int tm_hour;    /* 0~23 */
    int tm_mday;    /* 1~31 */
    int tm_mon;     /* 0~11 */
    int tm_year;    /* 1900 년 이후 */
    int tm_wday;    /* 0~6, 일요일 = 0 */
    int tm_yday;    /* 0~365 */
    int tm_isdst;   /* 항상 0 (UTC) */
};

clock_t clock(void);
double difftime(time_t time1, time_t time0);
time_t mktime(struct tm *timeptr);
time_t time(time_t *timer);

char *asctime(const struct tm *timeptr);
char *ctime(const time_t *timer);
struct tm *gmtime(const time_t *timer);
struct tm *localtime(const time_t *timer);
size_t strftime(char *s, size_t maxsize, const char *format, const struct tm *timeptr);

#include "_nl_features.h"
#if _NL_POSIX
struct timespec {
    time_t tv_sec;
    long tv_nsec;
};
typedef int clockid_t;
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
int clock_gettime(clockid_t clock_id, struct timespec *tp);
int nanosleep(const struct timespec *req, struct timespec *rem);   /* 해상도는 커널 타이머 (10ms) */
#endif

#endif /* _NL_TIME_H */
