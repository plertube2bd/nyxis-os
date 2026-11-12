/*
 * init - 첫 사용자 프로세스 (커널이 app:/init.run 을 uid 0 으로 실행한다)
 *
 * 하는 일:
 *  1. svc:/sys/init 채널을 등록한다 (누구나 보낼 수 있음). 다른 프로세스는 여기로 요청을 보낸다.
 *  2. 콘솔 로그인(app:/login.run)을 띄우고, 끝나면(죽으면) 다시 띄운다.
 *  3. NxPoll 로 "요청 도착", "login 종료", "sudo 로 띄운 명령 종료" 를 함께 기다린다 (바쁜 대기 없음).
 *
 * 요청 (메시지 본문):
 *   "poweroff"  시스템을 끈다. 정책: 이 기계의 콘솔에 로그인한 사용자가 끄는 것을 허용한다 —
 *               지금 이 OS 에는 원격 접속이 없으므로 메시지를 보낼 수 있는 프로세스는 모두 로컬
 *               세션에서 시작된 것이다. 커널은 메시지에 보낸 쪽의 uid/pid 를 붙여 주므로 정책을 바꾸고
 *               싶으면 여기서 info.sender_uid 로 거르면 된다. 실제로 끄는 일(NxPowerOff)은 uid 0 만 할 수 있고,
 *               init 이 그 uid 0 이다.
 *   "sudo\0..." 명령을 root 로 실행한다 (app:/sudo.run 이 보낸다). 필드는 NUL 로 나뉜다:
 *               sudo, pgid, 사용자 이름, 답장 채널, 비밀번호, PWD, 실행 파일 경로, argv[0], argv[1], ...
 *               허용 조건: 보낸 uid 가 SUDOERS_PATH 에 있고, uid 0 이 아니면 비밀번호가 맞아야 한다
 *               (NxAuthenticate 로 확인하고, 토큰의 uid 가 보낸 uid 와 같아야 한다: 남의 비밀번호로는 안 된다).
 *               답장 채널은 "svc:/<그 사용자 이름>/" 아래여야 한다. 자식은 root 로, 콘솔 0/1/2 로, 보낸 쪽의
 *               프로세스 그룹에서 시작한다 (Ctrl-C 가 함께 끝낸다). 끝나면 답장 채널로 "exit <코드>" 를,
 *               거부/실패면 "deny <이유>" 를 보낸다. 비밀번호가 틀리면 NxAuthenticate 가 1초 잠들고 그동안
 *               init 의 다른 요청도 기다린다 (무차별 대입을 늦추는 효과도 된다).
 * init 이 끝나면 커널은 멈춘다 (패닉) — 로그인도 전원 끄기도 할 수 없게 되기 때문이다.
 * 설정 welcome (nx_setting_get: app:/init.run/welcome): 부팅 후 로그인 전에 보이는 문구 (기본 "Welcome to Nyxis OS!").
 */
#include "nyx.h"

#define LOGIN_PATH "app:/login.run"

/* sudo 를 쓸 수 있는 uid 목록: 한 줄에 uid 하나 (10진수), '#' 부터 줄 끝은 주석. 없으면 uid 0 만 */
#define SUDOERS_PATH  "roi:/app/0/init/sudoers.dat"
#define SUDOERS_MAX   1024UL
#define SUDO_FIELDS   (7 + 16)           /* 고정 필드 7 + argv 최대 16 */
#define SUDO_JOBS     8                  /* 동시에 실행 중인 sudo 명령 */

struct sudo_job {
    long proc;                           /* 프로세스 핸들 (-1 = 빈 칸) */
    long reply;                          /* 답장 채널 송신 핸들 */
};

static struct sudo_job g_jobs[SUDO_JOBS];

static long spawn_login(void)
{
    static const char *const argv[] = { "login", 0 };
    /* login 은 보호 프로세스: init 말고는 root 도 신호로 끝낼 수 없다 (로그인 화면이 사라지지 않게) */
    long h = nx_spawn_ex(LOGIN_PATH, argv, 0, NX_NO_TOKEN, NX_SPAWN_CRITICAL);

    if (h < 0) {
        nx_puts("init: cannot start " LOGIN_PATH " (status ");
        nx_puti(h);
        nx_puts(")\n");
    }
    return h;
}

/* 10진수 문자열 -> 값. 숫자가 아니거나 너무 크면 -1 */
static long parse_ulong(const char *s, unsigned long n)
{
    unsigned long v = 0;
    unsigned long i;

    if (n == 0 || n > 10)
        return -1;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10UL + (unsigned long)(s[i] - '0');
    }
    return v > 0xFFFFFFFEUL ? -1 : (long)v;
}

/* uid 가 sudoers 에 있는지. 파일을 요청마다 다시 읽는다 (root 가 고치면 바로 반영) */
static int sudo_allowed(unsigned long uid)
{
    char buf[SUDOERS_MAX];
    long len = nx_read_value(SUDOERS_PATH, buf, sizeof(buf));
    long i = 0;

    if (len < 0)
        return uid == 0;
    while (i < len) {
        long start;
        long end;

        while (i < len && (buf[i] == ' ' || buf[i] == '\t'))
            i++;
        start = i;
        while (i < len && buf[i] >= '0' && buf[i] <= '9')
            i++;
        end = i;
        while (i < len && buf[i] != '\n')
            i++;                         /* 나머지(주석, 공백)는 무시 */
        i++;
        if (end > start && parse_ulong(buf + start, (unsigned long)(end - start)) == (long)uid)
            return 1;
    }
    return 0;
}

static void sudo_reply(long reply, const char *text)
{
    if (reply >= 0)
        (void)nx_channel_send(reply, text, nx_strlen(text));
}

/* 숫자를 붙인 답장 ("exit 3", "exit -2") */
static void sudo_reply_num(long reply, const char *prefix, long v)
{
    char out[48];
    char tmp[21];
    unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
    unsigned long n = 0;
    unsigned long k = 0;

    while (prefix[n] != '\0' && n < 24) {
        out[n] = prefix[n];
        n++;
    }
    if (v < 0)
        out[n++] = '-';
    do {
        tmp[k++] = (char)('0' + (u % 10UL));
        u /= 10UL;
    } while (u);
    while (k)
        out[n++] = tmp[--k];
    out[n] = '\0';
    sudo_reply(reply, out);
}

static void handle_sudo(char *msg, unsigned long len, const struct nx_msginfo *info)
{
    const char *f[SUDO_FIELDS + 1];
    char env_pwd[NX_CHAN_MSG_MAX + 4];
    const char *envp[5];
    unsigned long nf = 0;
    unsigned long i;
    unsigned long start = 0;
    unsigned long nlen;
    long pgid;
    long reply;
    long proc;
    int slot = -1;

    /* 필드 나누기: 마지막 바이트는 NUL 이어야 한다 */
    if (len == 0 || msg[len - 1] != '\0')
        return;
    for (i = 0; i < len; i++) {
        if (msg[i] == '\0') {
            if (nf == SUDO_FIELDS)
                return;
            f[nf++] = msg + start;
            start = i + 1;
        }
    }
    f[nf] = 0;
    if (nf < 8)                          /* sudo pgid 이름 답장 비밀번호 PWD 경로 argv[0] */
        return;

    /* 답장 채널: svc:/<이름>/ 아래만 (남의 채널로 보내지 않는다). 이름은 아래에서 비밀번호로 확인한다 */
    nlen = nx_strlen(f[2]);
    if (nlen == 0 || nx_strncmp(f[3], "svc:/", 5) != 0 || nx_strncmp(f[3] + 5, f[2], nlen) != 0 ||
        f[3][5 + nlen] != '/')
        reply = -1;
    else
        reply = nx_open(f[3], NX_O_WRITE);
    if (reply < 0)
        return;

    if (!sudo_allowed(info->sender_uid)) {
        nx_puts("init: sudo denied for uid ");
        nx_putu(info->sender_uid);
        nx_puts(" (not in " SUDOERS_PATH ")\n");
        sudo_reply(reply, "deny not in sudoers");
        (void)nx_close(reply);
        return;
    }

    if (info->sender_uid != 0) {
        struct nx_identity id;
        long token = nx_authenticate(f[2], f[4]);
        int ok = 0;

        if (token >= 0) {
            ok = nx_token_identity(token, &id) == 0 && id.uid == info->sender_uid;
            (void)nx_close(token);
        }
        if (!ok) {
            sudo_reply(reply, "deny authentication failed");
            (void)nx_close(reply);
            return;
        }
    }

    for (i = 0; i < SUDO_JOBS; i++) {
        if (g_jobs[i].proc < 0) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        sudo_reply(reply, "deny too many sudo commands");
        (void)nx_close(reply);
        return;
    }

    /* 환경: root 신원 + 보낸 쪽의 현재 디렉터리 */
    envp[0] = "USER=root";
    envp[1] = "LOGNAME=root";
    envp[2] = "SHELL=app:/nysh.run";
    envp[3] = 0;
    if (f[5][0] != '\0') {
        nx_memcpy(env_pwd, "PWD=", 4);
        nx_memcpy(env_pwd + 4, f[5], nx_strlen(f[5]) + 1);
        envp[3] = env_pwd;
    }
    envp[4] = 0;

    proc = nx_spawn(f[6], f + 7, envp, NX_NO_TOKEN);
    if (proc < 0) {
        sudo_reply_num(reply, "deny cannot run ", proc);
        (void)nx_close(reply);
        return;
    }
    pgid = parse_ulong(f[1], nx_strlen(f[1]));
    if (pgid > 0)
        (void)nx_syscall(NX_SYS_PROCESS_GROUP, (unsigned long)proc, (unsigned long)pgid, 0, 0, 0);

    nx_puts("init: sudo by uid ");
    nx_putu(info->sender_uid);
    nx_puts(": ");
    nx_puts(f[6]);
    nx_puts("\n");
    g_jobs[slot].proc = proc;
    g_jobs[slot].reply = reply;
}

/* sudo 로 띄운 명령이 끝났다: 종료 코드를 답장하고 칸을 비운다 */
static void sudo_finish(int slot)
{
    int code = 0;

    if (nx_wait(g_jobs[slot].proc, 0, &code) < 0)
        code = -1;
    sudo_reply_num(g_jobs[slot].reply, "exit ", code);
    (void)nx_close(g_jobs[slot].reply);
    (void)nx_close(g_jobs[slot].proc);
    g_jobs[slot].proc = -1;
    g_jobs[slot].reply = -1;
}

static void handle_request(char *msg, unsigned long len, const struct nx_msginfo *info)
{
    if (len >= 5 && nx_memcmp(msg, "sudo", 5) == 0) {
        handle_sudo(msg, len, info);
        return;
    }

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
    int i;

    (void)argc;
    (void)argv;
    (void)envp;

    for (i = 0; i < SUDO_JOBS; i++) {
        g_jobs[i].proc = -1;
        g_jobs[i].reply = -1;
    }

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

    {
        /* 설정 welcome (app:/init.run/welcome, root 의 사용자 값 roi:/0/app/init/welcome 이 먼저). 없으면 기본 문구 */
        char welcome[512];

        if (nx_setting_get("init", "welcome", welcome, sizeof(welcome)) >= 0) {
            nx_puts(welcome);
            nx_puts("\n");
        } else {
            nx_puts("Welcome to Nyxis OS!\n");
        }
    }
    login = spawn_login();

    for (;;) {
        struct nx_pollfd fds[2 + SUDO_JOBS];
        int job_of[2 + SUDO_JOBS];          /* fds[k] 가 sudo 명령이면 그 칸, 아니면 -1 */
        unsigned long n = 0;
        unsigned long k;
        long r;

        if (ch >= 0) {
            fds[n].handle = (unsigned long)ch;
            fds[n].events = NX_POLL_IN;
            fds[n].revents = 0;
            job_of[n] = -1;
            n++;
        }
        if (login >= 0) {
            fds[n].handle = (unsigned long)login;
            fds[n].events = NX_POLL_EXIT;
            fds[n].revents = 0;
            job_of[n] = -1;
            n++;
        }
        for (i = 0; i < SUDO_JOBS; i++) {
            if (g_jobs[i].proc >= 0) {
                fds[n].handle = (unsigned long)g_jobs[i].proc;
                fds[n].events = NX_POLL_EXIT;
                fds[n].revents = 0;
                job_of[n] = i;
                n++;
            }
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

        for (k = 0; k < n; k++) {
            if (job_of[k] >= 0) {
                if (fds[k].revents & NX_POLL_EXIT)
                    sudo_finish(job_of[k]);
                continue;
            }
            if ((long)fds[k].handle == ch && (fds[k].revents & NX_POLL_IN)) {
                char msg[NX_CHAN_MSG_MAX];
                struct nx_msginfo info;
                long len = nx_channel_receive(ch, msg, sizeof(msg), &info, 0);

                if (len >= 0) {
                    handle_request(msg, (unsigned long)len, &info);
                    nx_wipe(msg, sizeof(msg));     /* sudo 비밀번호가 스택에 남지 않게 */
                }
            }
            if ((long)fds[k].handle == login && (fds[k].revents & NX_POLL_EXIT)) {
                (void)nx_close(login);
                (void)nx_sleep(200);       /* 계속 죽는 경우 콘솔이 넘치지 않도록 */
                login = spawn_login();
            }
        }
    }
}
