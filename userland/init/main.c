/*
 * init - 첫 사용자 프로세스 (커널이 app:/init.run 을 uid 0 으로 실행한다)
 *
 * 하는 일:
 *  1. svc:/sys/init 채널을 등록한다 (누구나 보낼 수 있음). 다른 프로세스는 여기로 요청을 보낸다.
 *  2. 콘솔 로그인(app:/login.run)을 띄우고, 끝나면(죽으면) 다시 띄운다.
 *  3. NxPoll 로 "요청 도착" 과 "login 종료" 를 동시에 기다린다 (바쁜 대기 없음).
 *
 * 요청 (메시지 본문):
 *   "poweroff"  시스템을 끈다. 정책: 이 기계의 콘솔에 로그인한 사용자가 끄는 것을 허용한다 —
 *               지금 이 OS 에는 원격 접속이 없으므로 메시지를 보낼 수 있는 프로세스는 모두 로컬
 *               세션에서 시작된 것이다. 커널은 메시지에 보낸 쪽의 uid/pid 를 붙여 주므로 정책을 바꾸고
 *               싶으면 여기서 info.sender_uid 로 거르면 된다. 실제로 끄는 일(NxPowerOff)은 uid 0 만 할 수 있고,
 *               init 이 그 uid 0 이다.
 * init 이 끝나면 커널은 멈춘다 (패닉) — 로그인도 전원 끄기도 할 수 없게 되기 때문이다.
 */
#include "nyx.h"

#define LOGIN_PATH "app:/login.run"

static long spawn_login(void)
{
    static const char *const argv[] = { "login", 0 };
    long h = nx_spawn(LOGIN_PATH, argv, 0, NX_NO_TOKEN);

    if (h < 0) {
        nx_puts("init: cannot start " LOGIN_PATH " (status ");
        nx_puti(h);
        nx_puts(")\n");
    }
    return h;
}

static void handle_request(const char *msg, unsigned long len, const struct nx_msginfo *info)
{
    if (len == 8 && nx_memcmp(msg, "poweroff", 8) == 0) {
        long r;

        nx_puts("init: power off requested by uid ");
        nx_putu(info->sender_uid);
        nx_puts(" (pid ");
        nx_putu(info->sender_pid);
        nx_puts(")\n");
        r = nx_power_off();
        nx_puts("init: power off failed (status ");
        nx_puti(r);
        nx_puts(")\n");
        return;
    }

    nx_puts("init: unknown request from uid ");
    nx_putu(info->sender_uid);
    nx_puts("\n");
}

int main(int argc, char **argv, char **envp)
{
    struct nx_identity id;
    long ch;
    long login;

    (void)argc;
    (void)argv;
    (void)envp;

    if (nx_identity(&id) != 0 || id.uid != 0) {
        nx_puts("init: must run as root\n");
        return 1;
    }

    ch = nx_channel_create("svc:/sys/init", NX_CHAN_PUBLIC);
    if (ch < 0) {
        nx_puts("init: cannot register svc:/sys/init (status ");
        nx_puti(ch);
        nx_puts(")\n");
    }

    login = spawn_login();

    for (;;) {
        struct nx_pollfd fds[2];
        unsigned long n = 0;
        unsigned long i;
        long r;

        if (ch >= 0) {
            fds[n].handle = (unsigned long)ch;
            fds[n].events = NX_POLL_IN;
            fds[n].revents = 0;
            n++;
        }
        if (login >= 0) {
            fds[n].handle = (unsigned long)login;
            fds[n].events = NX_POLL_EXIT;
            fds[n].revents = 0;
            n++;
        }
        if (n == 0) {
            (void)nx_sleep(1000);
            login = spawn_login();
            continue;
        }

        r = nx_poll(fds, n, NX_WAIT_INFINITE);
        if (r < 0) {
            (void)nx_sleep(100);
            continue;
        }

        for (i = 0; i < n; i++) {
            if ((long)fds[i].handle == ch && (fds[i].revents & NX_POLL_IN)) {
                char msg[NX_CHAN_MSG_MAX];
                struct nx_msginfo info;
                long len = nx_channel_receive(ch, msg, sizeof(msg), &info, 0);

                if (len >= 0)
                    handle_request(msg, (unsigned long)len, &info);
            }
            if ((long)fds[i].handle == login && (fds[i].revents & NX_POLL_EXIT)) {
                (void)nx_close(login);
                (void)nx_sleep(200);       /* 계속 죽는 경우 콘솔이 넘치지 않도록 */
                login = spawn_login();
            }
        }
    }
}
