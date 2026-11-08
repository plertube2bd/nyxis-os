/*
 * sys/time.h - gettimeofday (POSIX, 오래된 인터페이스지만 많은 프로그램이 쓴다)
 *
 * 값은 clock_gettime(CLOCK_REALTIME) 과 같다 (CMOS RTC, 초 단위 + 부팅 후 경과로 보정). 시간대 인자는 무시한다.
 */
#ifndef _NL_SYS_TIME_H
#define _NL_SYS_TIME_H

#include <sys/types.h>
#include "../_nl_types.h"

struct timeval {
    time_t tv_sec;
    long tv_usec;
};

int gettimeofday(struct timeval *tv, void *tz);

#endif /* _NL_SYS_TIME_H */
