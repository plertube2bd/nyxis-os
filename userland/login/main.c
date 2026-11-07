/*
 * login - 콘솔 로그인 (init 이 uid 0 으로 띄운다)
 *
 *   nyxis os login: tamier
 *   password:              (에코 꺼짐)
 *
 * 커널(NxAuthenticate)이 roi:/users 의 해시로 검증하고 일회용 토큰을 준다. login 은 그 토큰으로
 * app:/nysh.run 을 실행한다 (셸은 그 사용자의 신원으로 시작하고 토큰은 소모된다). 셸이 끝나면
 * 다시 로그인 프롬프트로 돌아간다 — 다시 쓰려면 다시 인증해야 한다.
 * 비밀번호가 틀리면 커널이 1초 지연시킨다 (사용자가 없어도 같은 메시지와 같은 지연).
 */
#include "nyx.h"

#define SHELL_PATH "app:/nysh.run"

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    for (;;) {
        char name[64];
        char password[130];
        static const char *const shell_argv[] = { "nysh", 0 };
        long token;
        long shell;
        long n;
        int code = 0;

        nx_puts("\nnyxis os login: ");
        n = nx_readline(name, sizeof(name));
        if (n <= 0)
            continue;                      /* 빈 줄 / EOF */

        nx_puts("password: ");
        (void)nx_set_echo(0);
        n = nx_readline(password, sizeof(password));
        (void)nx_set_echo(1);
        if (n < 0) {
            nx_wipe(password, sizeof(password));
            continue;
        }

        token = nx_authenticate(name, password);
        nx_wipe(password, sizeof(password));
        if (token < 0) {
            nx_puts("Login incorrect\n");
            continue;
        }

        shell = nx_spawn(SHELL_PATH, shell_argv, 0, token);
        if (shell < 0) {
            nx_puts("login: cannot start " SHELL_PATH " (status ");
            nx_puti(shell);
            nx_puts(")\n");
            (void)nx_close(token);
            continue;
        }

        (void)nx_wait(shell, NX_WAIT_INFINITE, &code);
        (void)nx_close(shell);
    }
}
