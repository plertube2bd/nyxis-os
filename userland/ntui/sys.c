/*
 * sys.c - 세션 정보: 사용자, 호스트, 시간대 (nx64api: nx64_whoami, nx64_info)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <nx64.h>

#include "ntui.h"

uid_t uid;
char username[64];
int utcOffset;                          /* UTC 오프셋 (분). info:/utc_offset */
char timezoneName[64];                  /* info:/timezone */
char hostname[64];                      /* info:/hostname */
char isRoot;

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
    struct nx64_identity me;
    char buf[16];

    /* 커널이 아는 신원. isRoot 는 화면 표시용: 권한 판단은 커널이 한다 */
    if (nx64_whoami(&me) < 0) {
        me.uid = (unsigned long)getuid();
        strcpy(me.name, "unknown");
    }
    uid = (uid_t)me.uid;
    isRoot = uid == 0;
    strncpy(username, me.name[0] != '\0' ? me.name : "unknown", sizeof(username) - 1);
    username[sizeof(username) - 1] = '\0';

    /* timezone */
    if (nx64_info("timezone", timezoneName, sizeof(timezoneName)) <= 0) {
        fprintf(stderr, "read timezone failed.\n");
        fprintf(stderr, "using default timezone: %s\n", timezoneNameDefault);
        strcpy(timezoneName, timezoneNameDefault);
    }

    if (nx64_info("hostname", hostname, sizeof(hostname)) <= 0)
        strcpy(hostname, hostnameDefault);

    utcOffset = utcOffsetDefault;
    if (nx64_info("utc_offset", buf, sizeof(buf)) <= 0 || parseUtcOffset(buf, &utcOffset) < 0)
        utcOffset = utcOffsetDefault;

    return 0;
}
