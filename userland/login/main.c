/*
 * login - 콘솔 로그인 (init 이 uid 0 으로 띄운다)
 *
 *   nyxis os login: tamier
 *   password:              (에코 꺼짐)
 *
 * 커널(NxAuthenticate)이 roi:/users 의 해시로 검증하고 일회용 토큰을 준다. login 은 토큰이 담은 uid 로
 * roi:/<uid>/shell (관리자가 정한 로그인 셸, 사용자는 읽기만)을 읽고, 그 셸을 토큰으로 실행한다 (셸은 그
 * 사용자의 신원으로 시작하고 토큰은 소모된다). 값이 없거나 app:/ 실행 파일이 아니면 app:/nysh.run. 셸이 끝나면
 * 다시 로그인 프롬프트로 돌아간다 — 다시 쓰려면 다시 인증해야 한다.
 * 비밀번호가 틀리면 커널이 1초 지연시킨다 (사용자가 없어도 같은 메시지와 같은 지연).
 */
#include "nyx.h"

#define DEFAULT_SHELL "app:/nysh.run"
#define SHELL_MAX 128

/* 셸 경로 검사: "app:/" 로 시작하고 그 뒤가 비어 있지 않으며 공백/제어 문자가 없다 */
static int valid_shell(const char *p)
{
    unsigned long i;

    if (nx_strncmp(p, "app:/", 5) != 0 || p[5] == '\0')
        return 0;
    for (i = 0; p[i]; i++) {
        if ((unsigned char)p[i] <= ' ' || (unsigned char)p[i] >= 0x7F)
            return 0;
    }
    return 1;
}

/* 토큰의 uid 로 roi:/<uid>/shell 을 읽어 shell 에 넣는다 (실패하면 기본 셸) */
static void choose_shell(long token, char *shell, unsigned long max)
{
    struct nx_identity id;
    char path[32] = "roi:/";
    char digits[11];
    unsigned long uid;
    int n = 0;
    int pos = 5;

    if (nx_token_identity(token, &id) == 0) {
        uid = id.uid;
        do {
            digits[n++] = (char)('0' + uid % 10);
            uid /= 10;
        } while (uid);
        while (n > 0)
            path[pos++] = digits[--n];
        nx_memcpy(path + pos, "/shell", 7);
        if (nx_read_value(path, shell, max) > 0 && valid_shell(shell))
            return;
    }
    nx_memcpy(shell, DEFAULT_SHELL, sizeof(DEFAULT_SHELL));
}

/* "app:/dir/name.run" -> argv[0] = "name" */
static void shell_name(const char *shell, char *name, unsigned long max)
{
    const char *base = shell;
    unsigned long n;
    const char *p;

    for (p = shell; *p; p++) {
        if (*p == '/')
            base = p + 1;
    }
    n = nx_strlen(base);
    if (n > 4 && nx_strcmp(base + n - 4, ".run") == 0)
        n -= 4;
    if (n >= max)
        n = max - 1;
    nx_memcpy(name, base, n);
    name[n] = '\0';
}

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    for (;;) {
        char name[64];
        char password[130];
        char shell_path[SHELL_MAX];
        char shell_arg0[SHELL_MAX];
        const char *shell_argv[2];
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

        choose_shell(token, shell_path, sizeof(shell_path));
        shell_name(shell_path, shell_arg0, sizeof(shell_arg0));
        shell_argv[0] = shell_arg0;
        shell_argv[1] = 0;
        shell = nx_spawn(shell_path, shell_argv, 0, token);
        if (shell < 0 && nx_strcmp(shell_path, DEFAULT_SHELL) != 0) {
            /* 정해진 셸을 못 띄우면(없는 파일 등) 기본 셸로 (실패한 spawn 은 토큰을 소모하지 않는다) */
            nx_puts("login: cannot start ");
            nx_puts(shell_path);
            nx_puts(", using " DEFAULT_SHELL "\n");
            nx_memcpy(shell_path, DEFAULT_SHELL, sizeof(DEFAULT_SHELL));
            nx_memcpy(shell_arg0, "nysh", 5);
            shell = nx_spawn(shell_path, shell_argv, 0, token);
        }
        if (shell < 0) {
            nx_puts("login: cannot start ");
            nx_puts(shell_path);
            nx_puts(" (status ");
            nx_puti(shell);
            nx_puts(")\n");
            (void)nx_close(token);
            continue;
        }

        (void)nx_wait(shell, NX_WAIT_INFINITE, &code);
        (void)nx_close(shell);
    }
}
