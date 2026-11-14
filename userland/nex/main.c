/*
 * nex - init 의 서비스(유닛) 관리 명령 (systemctl 과 비슷하다)
 *
 *   nex [list]                    유닛 목록
 *   nex status [유닛]             상태와 최근 로그 10줄 (유닛이 없으면 목록)
 *   nex start|stop|restart 유닛...
 *   nex enable|disable [--now] 유닛...   부팅 때 시작할지 (--now: 지금 시작/멈춤도)
 *   nex reload                    유닛 파일을 다시 읽는다
 *   nex cat 유닛                  유닛 파일 보기
 *   nex poweroff                  전원 끄기
 * 바꾸는 명령(start stop restart enable disable reload)은 root 만: elev nex start journald
 *
 * init 과의 약속은 userland/init/service.c 머리말: "nex\0<명령>\0<유닛>\0" 을 svc:/sys/init 에 보내면서
 * 답장 채널(svc:/<나>/nex-<pid>)의 송신 끝을 붙인다. 답장은 'T'+글 ..., 'E'+결과.
 * 종료 코드: 0 성공, 1 실패, 2 유닛 없음, 3 권한 없음, 4 다른 명령이 진행 중, 5 사용법. status 는 active 가 아니면 1
 */
#include <nx64.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define INIT_SVC   "svc:/sys/init"
#define ANSWER_MAX 8192
#define MSG_MAX    256

static int g_color;

static const char *c_on(const char *code)
{
    return g_color ? code : "";
}

static const char *c_off(void)
{
    return g_color ? "\033[0m" : "";
}

/* init 에게 묻는다. 답장 글은 out 에 (NUL 로 끝남), 반환은 결과 코드 (init 과 닿지 못하면 1) */
static int ask_init(const char *cmd, const char *unit, char *out, size_t cap)
{
    char name[48];
    char msg[MSG_MAX];
    size_t n = 0;
    size_t used = 0;
    long recv;
    int recv_fd;
    int send_fd;
    int code = -1;

    out[0] = '\0';
    snprintf(name, sizeof(name), "nex-%ld", (long)getpid());
    recv = nx64_service_create(name, 0);
    if (recv < 0) {
        fprintf(stderr, "nex: cannot create reply channel: %s\n", strerror(errno));
        return 1;
    }
    recv_fd = nx64_service_fd(recv);    /* close() 로 닫으려고 (닫으면 채널 이름도 사라진다) */
    send_fd = nx64_service_connect(name);
    if (send_fd < 0) {
        fprintf(stderr, "nex: cannot open reply channel: %s\n", strerror(errno));
        close(recv_fd);
        return 1;
    }
    memcpy(msg, "nex", 4);
    n = 4;
    if (strlen(cmd) + strlen(unit) + 2 + n > sizeof(msg)) {
        fprintf(stderr, "nex: name too long\n");
        close(send_fd);
        close(recv_fd);
        return 5;
    }
    memcpy(msg + n, cmd, strlen(cmd) + 1);
    n += strlen(cmd) + 1;
    memcpy(msg + n, unit, strlen(unit) + 1);
    n += strlen(unit) + 1;
    if (nx64_service_send_fd(INIT_SVC, msg, n, send_fd) < 0) {
        fprintf(stderr, "nex: cannot reach init (%s): %s\n", INIT_SVC, strerror(errno));
        close(send_fd);
        close(recv_fd);
        return 1;
    }
    close(send_fd);

    while (code < 0) {
        char buf[MSG_MAX + 1];
        struct nx64_sender from;
        long len = nx64_service_receive(recv, buf, MSG_MAX, &from, 600000);

        if (len < 0) {
            fprintf(stderr, "nex: no answer from init: %s\n", strerror(errno));
            code = 1;
            break;
        }
        if (from.uid != 0 || len < 1)
            continue;                    /* init(root) 이 보낸 것만 */
        if (buf[0] == 'T') {
            size_t k = (size_t)len - 1;

            if (used + k >= cap)
                k = cap - 1 - used;
            memcpy(out + used, buf + 1, k);
            used += k;
            out[used] = '\0';
        } else if (buf[0] == 'E') {
            buf[len] = '\0';
            code = atoi(buf + 1);
        }
    }
    close(recv_fd);
    return code;
}

/* 답장 글이 오류 설명이면 그대로 stderr 에 */
static void show_error(const char *text)
{
    if (text[0] != '\0')
        fprintf(stderr, "nex: %s%s", text, text[strlen(text) - 1] == '\n' ? "" : "\n");
}

/* "키\t값" 줄들에서 값을 찾는다 */
static const char *field(const char *text, const char *key, char *buf, size_t cap)
{
    size_t kl = strlen(key);
    const char *p = text;

    while (*p != '\0') {
        const char *e = strchr(p, '\n');
        size_t ll = e != NULL ? (size_t)(e - p) : strlen(p);

        if (ll > kl && strncmp(p, key, kl) == 0 && p[kl] == '\t') {
            size_t vl = ll - kl - 1;

            if (vl >= cap)
                vl = cap - 1;
            memcpy(buf, p + kl + 1, vl);
            buf[vl] = '\0';
            return buf;
        }
        if (e == NULL)
            break;
        p = e + 1;
    }
    buf[0] = '\0';
    return buf;
}

static void duration(long ms, char *buf, size_t cap)
{
    long s = ms / 1000;

    if (s < 60)
        snprintf(buf, cap, "%lds", s);
    else if (s < 3600)
        snprintf(buf, cap, "%ldmin %lds", s / 60, s % 60);
    else if (s < 86400)
        snprintf(buf, cap, "%ldh %ldmin", s / 3600, (s / 60) % 60);
    else
        snprintf(buf, cap, "%ldd %ldh", s / 86400, (s / 3600) % 24);
}

static const char *state_color(const char *state)
{
    if (strcmp(state, "active") == 0)
        return c_on("\033[1;32m");
    if (strcmp(state, "failed") == 0)
        return c_on("\033[1;31m");
    if (strcmp(state, "stopping") == 0 || strcmp(state, "restarting") == 0)
        return c_on("\033[1;33m");
    return "";
}

static int cmd_list(void)
{
    char text[ANSWER_MAX];
    int code = ask_init("list", "", text, sizeof(text));
    char *p = text;
    int count = 0;

    if (code != 0) {
        show_error(text);
        return code;
    }
    printf("%s%-16s %-11s %6s  %-9s %s%s\n", c_on("\033[1m"), "UNIT", "STATE", "PID", "BOOT", "DESCRIPTION",
           c_off());
    while (*p != '\0') {
        char *f[5];
        char *e = strchr(p, '\n');
        int k;

        if (e != NULL)
            *e = '\0';
        for (k = 0; k < 5; k++) {
            f[k] = p;
            p = strchr(p, '\t');
            if (p == NULL) {
                p = f[k] + strlen(f[k]);
                for (k++; k < 5; k++)
                    f[k] = p;
                break;
            }
            *p++ = '\0';
        }
        printf("%-16s %s%-11s%s %6s  %-9s %s\n", f[0], state_color(f[1]), f[1], state_color(f[1])[0] ? c_off() : "",
               strcmp(f[2], "0") == 0 ? "-" : f[2], f[3], f[4]);
        count++;
        if (e == NULL)
            break;
        p = e + 1;
    }
    printf("\n%d unit%s\n", count, count == 1 ? "" : "s");
    return 0;
}

/* 최근 로그 (journalman -u 유닛 -n lines 와 같다) */
static void show_journal(const char *unit, long lines)
{
    struct nx64_journal_query q;
    char host[64];
    FILE *f;
    char *line = NULL;
    size_t lcap = 0;
    int fd;
    int shown = 0;

    memset(&q, 0, sizeof(q));
    q.unit = unit;
    q.max_prio = -1;
    q.lines = lines;
    q.this_boot = 1;
    fd = nx64_journal_open(&q);
    if (fd < 0) {
        printf("\n(journal is not available: %s)\n", strerror(errno));
        return;
    }
    if (nx64_info("hostname", host, sizeof(host)) < 0)
        strcpy(host, "nyxis");
    f = fdopen(fd, "r");
    if (f == NULL) {
        close(fd);
        return;
    }
    while (getline(&line, &lcap, f) > 0) {
        struct nx64_journal_entry e;
        char out[512];

        if (nx64_journal_parse(line, &e) < 0)
            continue;
        if (shown++ == 0)
            printf("\n");
        nx64_journal_format(&e, host, out, sizeof(out));
        if (e.prio <= NX64_LOG_ERR)
            printf("%s%s%s\n", c_on("\033[1;31m"), out, c_off());
        else if (e.prio == NX64_LOG_WARNING)
            printf("%s%s%s\n", c_on("\033[33m"), out, c_off());
        else
            printf("%s\n", out);
    }
    free(line);
    fclose(f);
    if (shown == 0 && getuid() != 0)
        printf("\n(only root can see system logs: elev nex status %s)\n", unit);
}

static int cmd_status(const char *unit)
{
    char text[ANSWER_MAX];
    char v[160];
    char state[24];
    char dur[32];
    char pid[24];
    char result[32];
    int code = ask_init("status", unit, text, sizeof(text));

    if (code != 0) {
        show_error(text);
        return code;
    }
    field(text, "state", state, sizeof(state));
    field(text, "result", result, sizeof(result));
    duration(atol(field(text, "since_ms", v, sizeof(v))), dur, sizeof(dur));
    field(text, "pid", pid, sizeof(pid));

    printf("%s*%s %s", state_color(state), state_color(state)[0] ? c_off() : "", unit);   /* 콘솔 글꼴은 ASCII 만 */
    if (field(text, "description", v, sizeof(v))[0] != '\0')
        printf(" - %s", v);
    printf("\n");
    printf("     Loaded: %s", field(text, "source", v, sizeof(v)));
    printf("; %s", field(text, "enabled", v, sizeof(v)));
    printf(" (boot default: %s)\n", field(text, "boot", v, sizeof(v)));
    printf("     Active: %s%s", state_color(state), state);
    if (strcmp(state, "active") == 0)
        printf(" (running)");
    else if (strcmp(state, "inactive") == 0)
        printf(" (dead)");
    else if (strcmp(state, "restarting") == 0)
        printf(" (auto-restart)");
    else if (strcmp(state, "failed") == 0 && result[0] != '\0')
        printf(" (Result: %s)", result);
    printf("%s for %s\n", state_color(state)[0] ? c_off() : "", dur);
    if (pid[0] != '\0')
        printf("   Main PID: %s\n", pid);
    printf("       Exec: %s", field(text, "exec", v, sizeof(v)));
    if (field(text, "args", v, sizeof(v))[0] != '\0')
        printf(" %s", v);
    printf("\n");
    printf("    Restart: %s", field(text, "restart", v, sizeof(v)));
    printf(" (restarted %s times)\n", field(text, "restarts", v, sizeof(v)));
    printf("     Output: %s\n", field(text, "output", v, sizeof(v)));
    if (field(text, "last_exit", v, sizeof(v))[0] != '\0')
        printf("  Last exit: %s%s%s%s\n", v, result[0] ? " (" : "", result, result[0] ? ")" : "");
    show_journal(unit, 10);
    return strcmp(state, "active") == 0 ? 0 : 1;
}

/* start/stop/restart/enable/disable 를 유닛마다 */
static int cmd_each(const char *cmd, int argc, char **argv, int now)
{
    int rc = 0;
    int i;

    if (argc == 0) {
        fprintf(stderr, "nex: %s: which unit? (nex list)\n", cmd);
        return 5;
    }
    for (i = 0; i < argc; i++) {
        char text[ANSWER_MAX];
        int code = ask_init(cmd, argv[i], text, sizeof(text));

        if (code != 0) {
            show_error(text);
            rc = code;
            continue;
        }
        if (now) {
            const char *then = strcmp(cmd, "enable") == 0 ? "start" : "stop";

            code = ask_init(then, argv[i], text, sizeof(text));
            if (code != 0) {
                show_error(text);
                rc = code;
            }
        }
    }
    return rc;
}

static int cmd_cat(const char *unit)
{
    char path[160];
    const char *dirs[2] = { "roi:/app/0/init/units", "app:/init.run/units" };
    int i;

    if (strchr(unit, '/') != NULL || unit[0] == '.' || unit[0] == '\0') {
        fprintf(stderr, "nex: bad unit name: %s\n", unit);
        return 5;
    }
    for (i = 0; i < 2; i++) {
        FILE *f;
        char buf[512];
        size_t n;

        snprintf(path, sizeof(path), "%s/%s", dirs[i], unit);
        f = fopen(path, "r");
        if (f == NULL)
            continue;
        printf("%s# %s%s\n", c_on("\033[1;34m"), path, c_off());
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            fwrite(buf, 1, n, stdout);
        fclose(f);
        return 0;
    }
    fprintf(stderr, "nex: no unit file for %s\n", unit);
    return 2;
}

static void usage(FILE *out)
{
    fprintf(out,
            "usage: nex [command] [unit...]\n"
            "  list                       list units (default)\n"
            "  status [unit]              show unit state and its recent log\n"
            "  start|stop|restart unit... control units (root: elev nex ...)\n"
            "  enable|disable [--now] unit...\n"
            "                             start at boot or not (--now: also start/stop now)\n"
            "  reload                     re-read unit files\n"
            "  cat unit                   show the unit file\n"
            "  poweroff                   power the system off\n"
            "unit files: app:/init.run/units/<name>, roi:/app/0/init/units/<name> (format: nex help-unit)\n");
}

static void usage_unit(void)
{
    printf("unit file (one 'key = value' per line, '#' comments):\n"
           "  description = text\n"
           "  exec        = app:/prog.run          (required)\n"
           "  args        = arguments (\"...\" groups words)\n"
           "  restart     = no | on-failure | always\n"
           "  restart_ms  = delay before restarting (1000)\n"
           "  stop_ms     = SIGTERM -> SIGKILL grace time (3000)\n"
           "  output      = journal | console       (stdout/stderr)\n"
           "  boot        = yes | no                start at boot (nex enable/disable overrides)\n"
           "  order       = 0..99                   boot order (50)\n"
           "add one: write roi:/app/0/init/units/<name> as root, then: elev nex reload\n");
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "list";

    g_color = isatty(1);
    if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0)
        return cmd_list();
    if (strcmp(cmd, "status") == 0) {
        int rc = 0;
        int i;

        if (argc < 3)
            return cmd_list();
        for (i = 2; i < argc; i++) {
            int r;

            if (i > 2)
                printf("\n");
            r = cmd_status(argv[i]);
            if (r != 0)
                rc = r;
        }
        return rc;
    }
    if (strcmp(cmd, "start") == 0 || strcmp(cmd, "stop") == 0 || strcmp(cmd, "restart") == 0)
        return cmd_each(cmd, argc - 2, argv + 2, 0);
    if (strcmp(cmd, "enable") == 0 || strcmp(cmd, "disable") == 0) {
        int now = argc > 2 && strcmp(argv[2], "--now") == 0;

        return cmd_each(cmd, argc - 2 - now, argv + 2 + now, now);
    }
    if (strcmp(cmd, "reload") == 0) {
        char text[ANSWER_MAX];
        int code = ask_init("reload", "", text, sizeof(text));

        if (code != 0)
            show_error(text);
        return code;
    }
    if (strcmp(cmd, "cat") == 0) {
        if (argc < 3) {
            usage(stderr);
            return 5;
        }
        return cmd_cat(argv[2]);
    }
    if (strcmp(cmd, "poweroff") == 0) {
        if (nx64_request_poweroff() < 0) {
            fprintf(stderr, "nex: cannot reach init: %s\n", strerror(errno));
            return 1;
        }
        return 0;
    }
    if (strcmp(cmd, "help-unit") == 0) {
        usage_unit();
        return 0;
    }
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        usage(stdout);
        return 0;
    }
    fprintf(stderr, "nex: unknown command: %s\n", cmd);
    usage(stderr);
    return 5;
}
