/*
 * poweroff - init 에게 시스템을 꺼 달라고 "요청" 한다
 *
 * 직접 끄지 않는다 (NxPowerOff 는 uid 0 만 가능). svc:/sys/init 채널로 "poweroff" 를 보내면 init(uid 0)이
 * 요청한 사용자를 확인하고 끈다. 커널이 메시지에 보낸 사람의 uid 를 붙이므로 사칭할 수 없다.
 */
#include "nyx.h"

int main(int argc, char **argv, char **envp)
{
    long h;
    long r;

    (void)argc;
    (void)argv;
    (void)envp;

    h = nx_open("svc:/sys/init", NX_O_WRITE);
    if (h < 0) {
        nx_puts("poweroff: init is not reachable (status ");
        nx_puti(h);
        nx_puts(")\n");
        return 1;
    }

    r = nx_channel_send(h, "poweroff", 8);
    (void)nx_close(h);
    if (r < 0) {
        nx_puts("poweroff: request failed (status ");
        nx_puti(r);
        nx_puts(")\n");
        return 1;
    }

    nx_puts("poweroff: request sent to init\n");
    return 0;
}
