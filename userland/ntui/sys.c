/*
 * sys.c - 세션 정보: 사용자, 시간대
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "ntui.h"

uid_t uid;
char username[64];
int utcOffset;                          /* UTC 오프셋 (분). info:/utc_offset */
char timezoneName[64];                  /* info:/timezone */
char isRoot;

/*
 * info:/ 같은 "파일 하나에 값 하나" 를 buf 에 읽는다. 끝의 줄바꿈은 뗀다.
 * 성공하면 0, 못 열거나 비어 있으면 -1 (buf 는 빈 문자열).
 */
static int readValue(const char *path, char *buf, size_t cap)
{
    int file;
    size_t len = 0;
    ssize_t got;

    buf[0] = '\0';
    file = open(path, O_RDONLY);
    if (file < 0)
        return -1;
    while (len < cap - 1 && (got = read(file, buf + len, cap - 1 - len)) > 0)
        len += (size_t)got;
    close(file);

    buf[len] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
    return buf[0] != '\0' ? 0 : -1;
}

/* "+HH:MM" / "-HH:MM" -> 분. 형식이 틀리면 -1 */
static int parseUtcOffset(const char *s, int *out)
{
    int sign;
    int hh;
    int mm;

    if (s[0] != '+' && s[0] != '-')
        return -1;
    sign = s[0] == '-' ? -1 : 1;
    if (strlen(s) != 6 || s[3] != ':' ||
        s[1] < '0' || s[1] > '9' || s[2] < '0' || s[2] > '9' ||
        s[4] < '0' || s[4] > '9' || s[5] < '0' || s[5] > '9')
        return -1;
    hh = (s[1] - '0') * 10 + (s[2] - '0');
    mm = (s[4] - '0') * 10 + (s[5] - '0');
    if (hh > 23 || mm > 59)
        return -1;
    *out = sign * (hh * 60 + mm);
    return 0;
}

int init(void)
{
    const char *name;
    char buf[16];

    /* init tty */
    if (ttyOpen() < 0) {
        fprintf(stderr, "can't open dev:/tty.\nthere is no console.\n");
        return -1;
    }

    /* userid, username. isRoot 는 화면 표시용: 권한 판단은 커널이 한다 */
    uid = getuid();
    isRoot = uid == 0;

    name = getlogin();
    if (name == NULL)
        name = "unknown";
    strncpy(username, name, sizeof(username) - 1);
    username[sizeof(username) - 1] = '\0';

    /* timezone */
    if (readValue("info:/timezone", timezoneName, sizeof(timezoneName)) < 0) {
        fprintf(stderr, "read timezone failed.\n");
        fprintf(stderr, "using default timezone: %s\n", timezoneNameDefault);
        strcpy(timezoneName, timezoneNameDefault);
    }

    utcOffset = utcOffsetDefault;
    if (readValue("info:/utc_offset", buf, sizeof(buf)) < 0 || parseUtcOffset(buf, &utcOffset) < 0)
        utcOffset = utcOffsetDefault;

    return 0;
}
