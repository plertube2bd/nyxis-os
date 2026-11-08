/*
 * sleep <초> - 정해진 시간만큼 잠든다 (소수 가능: "0.5")
 */
#include <stdlib.h>
#include <time.h>
#include "common.h"

int main(int argc, char **argv)
{
    struct timespec ts;
    char *end;
    double sec;

    cu_prog = "sleep";
    if (argc != 2) {
        cu_msg("usage: sleep <seconds>");
        return 2;
    }
    sec = strtod(argv[1], &end);
    if (end == argv[1] || *end != '\0' || sec < 0 || sec > 1e9) {
        cu_msg("invalid time '%s'", argv[1]);
        return 2;
    }
    ts.tv_sec = (time_t)sec;
    ts.tv_nsec = (long)((sec - (double)ts.tv_sec) * 1e9);
    if (ts.tv_nsec > 999999999L)
        ts.tv_nsec = 999999999L;
    while (nanosleep(&ts, &ts) != 0)
        ;
    return 0;
}
