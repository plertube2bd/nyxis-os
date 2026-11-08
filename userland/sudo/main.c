/*
 * sudo - 명령을 root 로 실행해 달라고 init 에게 "요청" 한다 (poweroff 와 같은 구조)
 *
 *   sudo <명령> [인자...]      명령: 이름만이면 app:/<이름>.run, ':' 이 있으면 그 경로
 *
 * 직접 root 가 되지 않는다. 비밀번호를 묻고, 답장 채널 svc:/<사용자>/sudo-<pid> 를 만든 뒤 svc:/sys/init 로
 * 요청을 보낸다. init(uid 0)이 보낸 uid 가 roi:/app/0/init/sudoers.dat 에 있는지, 비밀번호가 맞는지 확인하고
 * 명령을 root 로 띄운다. 명령이 끝나면 init 이 답장 채널로 종료 코드를 보내 주고, sudo 는 그 코드로 끝난다.
 * 명령은 sudo 와 같은 프로세스 그룹에서 돌므로 Ctrl-C 가 둘 다 끝낸다. 요청 형식은 init/main.c 머리말.
 *
 * 제한: 요청 전체(비밀번호, 경로, 인자)가 채널 메시지 하나(NX_CHAN_MSG_MAX = 256 바이트)에 들어가야 한다.
 */
#include "nyx.h"

#define MAX_ARGS 16

static unsigned long g_len;
static char g_msg[NX_CHAN_MSG_MAX];

/* 요청에 필드 하나 (NUL 포함). 넘치면 -1 */
static int put_field(const char *s)
{
    unsigned long n = nx_strlen(s) + 1;

    if (g_len + n > sizeof(g_msg))
        return -1;
    nx_memcpy(g_msg + g_len, s, n);
    g_len += n;
    return 0;
}

/* 10진수 문자열 (버퍼는 21 바이트 이상) */
static void utoa(unsigned long v, char *out)
{
    char tmp[21];
    unsigned long k = 0;
    unsigned long n = 0;

    do {
        tmp[k++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    } while (v);
    while (k)
        out[n++] = tmp[--k];
    out[n] = '\0';
}

static int has_char(const char *s, char c)
{
    while (*s) {
        if (*s++ == c)
            return 1;
    }
    return 0;
}

/* envp 에서 NAME= 의 값 (없으면 "") */
static const char *env_get(char **envp, const char *name)
{
    unsigned long n = nx_strlen(name);

    while (envp && *envp) {
        if (nx_strncmp(*envp, name, n) == 0 && (*envp)[n] == '=')
            return *envp + n + 1;
        envp++;
    }
    return "";
}

static int fail(const char *what, long status)
{
    nx_puts("sudo: ");
    nx_puts(what);
    if (status) {
        nx_puts(" (status ");
        nx_puti(status);
        nx_puts(")");
    }
    nx_puts("\n");
    return 1;
}

int main(int argc, char **argv, char **envp)
{
    struct nx_identity id;
    struct nx_procinfo pi;
    struct nx_msginfo info;
    char path[128];
    char reply_path[96];
    char num[24];
    char password[128];
    char answer[64];
    long pgid;
    long reply;
    long init;
    long r;
    int i;

    if (argc < 2) {
        nx_puts("usage: sudo <command> [args...]\n");
        return 2;
    }
    if (argc - 1 > MAX_ARGS)
        return fail("too many arguments", 0);
    if (nx_identity(&id) != 0 || nx_process_info(NX_PID_SELF, &pi) != 0)
        return fail("cannot read identity", 0);

    /* 실행 파일 경로 */
    if (has_char(argv[1], ':')) {
        if (nx_strlen(argv[1]) >= sizeof(path))
            return fail("path too long", 0);
        nx_memcpy(path, argv[1], nx_strlen(argv[1]) + 1);
    } else {
        unsigned long n = nx_strlen(argv[1]);

        if (n == 0 || has_char(argv[1], '/') || n + 10 >= sizeof(path))
            return fail("bad command name", 0);
        nx_memcpy(path, "app:/", 5);
        nx_memcpy(path + 5, argv[1], n);
        nx_memcpy(path + 5 + n, ".run", 5);
    }

    /* 답장 채널: svc:/<이름>/sudo-<pid> (같은 uid 와 root 만 보낼 수 있다) */
    if (nx_strlen(id.name) + 32 >= sizeof(reply_path))
        return fail("user name too long", 0);
    nx_memcpy(reply_path, "svc:/", 5);
    nx_memcpy(reply_path + 5, id.name, nx_strlen(id.name));
    nx_memcpy(reply_path + 5 + nx_strlen(id.name), "/sudo-", 7);
    utoa(pi.pid, num);
    nx_memcpy(reply_path + nx_strlen(reply_path), num, nx_strlen(num) + 1);

    init = nx_open("svc:/sys/init", NX_O_WRITE);
    if (init < 0)
        return fail("init is not reachable", init);
    reply = nx_channel_create(reply_path, 0);
    if (reply < 0)
        return fail("cannot create reply channel", reply);

    /* 비밀번호 (root 는 묻지 않는다) */
    password[0] = '\0';
    if (id.uid != 0) {
        nx_puts("[sudo] password for ");
        nx_puts(id.name);
        nx_puts(": ");
        (void)nx_set_echo(0);
        r = nx_readline(password, sizeof(password));
        (void)nx_set_echo(1);
        if (r < 0) {
            nx_wipe(password, sizeof(password));
            return fail("no password", 0);
        }
    }

    pgid = nx_syscall(NX_SYS_PROCESS_GROUP, NX_PID_SELF, NX_PGID_QUERY, 0, 0, 0);
    utoa(pgid > 0 ? (unsigned long)pgid : 0UL, num);

    g_len = 0;
    r = put_field("sudo") | put_field(num) | put_field(id.name) | put_field(reply_path) |
        put_field(password) | put_field(env_get(envp, "PWD")) | put_field(path);
    for (i = 1; i < argc && r == 0; i++)
        r = put_field(argv[i]);
    nx_wipe(password, sizeof(password));
    if (r != 0) {
        nx_wipe(g_msg, sizeof(g_msg));
        return fail("command line too long", 0);
    }

    r = nx_channel_send(init, g_msg, g_len);
    nx_wipe(g_msg, sizeof(g_msg));
    (void)nx_close(init);
    if (r < 0)
        return fail("request failed", r);

    /* 답장: init(uid 0) 이 보낸 것만 믿는다 */
    for (;;) {
        r = nx_channel_receive(reply, answer, sizeof(answer) - 1, &info, NX_WAIT_INFINITE);
        if (r < 0)
            return fail("no answer from init", r);
        if (info.sender_uid == 0)
            break;
    }
    answer[r] = '\0';
    (void)nx_close(reply);

    if (nx_strncmp(answer, "exit ", 5) == 0) {
        const char *p = answer + 5;
        int neg = *p == '-';
        long code = 0;

        if (neg)
            p++;
        while (*p >= '0' && *p <= '9')
            code = code * 10 + (*p++ - '0');
        return (int)(neg ? -code : code);
    }
    if (nx_strncmp(answer, "deny ", 5) == 0)
        return fail(answer + 5, 0);
    return fail("unexpected answer from init", 0);
}
