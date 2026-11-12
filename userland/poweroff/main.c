/*
 * poweroff - init 에게 시스템을 꺼 달라고 "요청" 한다 (nx64api: nx64_request_poweroff)
 *
 * 직접 끄지 않는다 (NxPowerOff 는 uid 0 만 가능). svc:/sys/init 채널로 "poweroff" 를 보내면 init(uid 0)이
 * 요청한 사용자를 확인하고 끈다. 커널이 메시지에 보낸 사람의 uid 를 붙이므로 사칭할 수 없다.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <nx64.h>

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        fputs("usage: poweroff\n", stderr);
        return 2;
    }
    if (nx64_request_poweroff() < 0) {
        fprintf(stderr, "poweroff: request to init failed: %s\n", strerror(errno));
        return 1;
    }
    puts("poweroff: request sent to init");
    return 0;
}
