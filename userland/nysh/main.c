/*
 * nysh - 아주 단순한 셸
 *
 *   tamier@nyxis$ whoami
 *   tamier
 *   tamier@nyxis$ poweroff
 *
 * 한 줄을 공백으로 나눠 첫 단어를 명령으로 본다.
 *   내장 명령: exit, whoami, help
 *   그 외: app:/<명령>.run 을 같은 인자로 실행하고 끝날 때까지 기다린다 (':' 가 있으면 경로 그대로).
 * 파이프, 리다이렉션, 변수, 따옴표, 현재 디렉터리 같은 기능은 없다.
 */
#include "nyx.h"

#define LINE_MAX_LEN 256
#define ARG_MAX_COUNT 16

static int split(char *line, char **argv)
{
    int n = 0;
    char *p = line;

    while (*p && n < ARG_MAX_COUNT - 1) {
        while (*p == ' ' || *p == '\t')
            *p++ = '\0';
        if (!*p)
            break;
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    argv[n] = 0;
    return n;
}

static int has_colon(const char *s)
{
    while (*s) {
        if (*s++ == ':')
            return 1;
    }
    return 0;
}

static void run(char **argv)
{
    char path[LINE_MAX_LEN + 16];
    unsigned long len;
    long h;
    int code = 0;

    if (has_colon(argv[0])) {
        len = nx_strlen(argv[0]);
        if (len >= sizeof(path))
            return;
        nx_memcpy(path, argv[0], len + 1);
    } else {
        len = nx_strlen(argv[0]);
        if (len + 10 >= sizeof(path))
            return;
        nx_memcpy(path, "app:/", 5);
        nx_memcpy(path + 5, argv[0], len);
        nx_memcpy(path + 5 + len, ".run", 5);
    }

    h = nx_spawn(path, (const char *const *)argv, 0, NX_NO_TOKEN);
    if (h < 0) {
        nx_puts("nysh: ");
        nx_puts(argv[0]);
        nx_puts(h == NX_E_NOT_FOUND || h == NX_ERR(29) ? ": command not found\n" : ": cannot run (status ");
        if (!(h == NX_E_NOT_FOUND || h == NX_ERR(29))) {
            nx_puti(h);
            nx_puts(")\n");
        }
        return;
    }
    (void)nx_wait(h, NX_WAIT_INFINITE, &code);
    (void)nx_close(h);
    if (code != 0) {
        nx_puts("[exit ");
        nx_puti(code);
        nx_puts("]\n");
    }
}

int main(int argc, char **argv, char **envp)
{
    struct nx_identity id;
    char line[LINE_MAX_LEN];
    char *args[ARG_MAX_COUNT];

    (void)argc;
    (void)argv;
    (void)envp;

    if (nx_identity(&id) != 0) {
        nx_puts("nysh: cannot get identity\n");
        return 1;
    }

    for (;;) {
        long n;

        nx_puts(id.name);
        nx_puts(id.uid == 0 ? "@nyxis# " : "@nyxis$ ");
        n = nx_readline(line, sizeof(line));
        if (n < 0) {
            nx_puts("\n");
            return 0;                      /* EOF (Ctrl-D): 로그아웃 */
        }
        if (split(line, args) == 0)
            continue;

        if (nx_strcmp(args[0], "exit") == 0 || nx_strcmp(args[0], "logout") == 0)
            return 0;
        if (nx_strcmp(args[0], "whoami") == 0) {
            nx_puts(id.name);
            nx_puts("\n");
            continue;
        }
        if (nx_strcmp(args[0], "help") == 0) {
            nx_puts("built-in: exit, whoami, help\n");
            nx_puts("other commands run app:/<name>.run (e.g. poweroff)\n");
            continue;
        }
        run(args);
    }
}
