/*
 * nx64_journal.c - nx64api 의 저널 부분 (규칙은 nx64.h): journald(svc:/sys/journal) 에 쓰기와 읽기
 *
 * journald 에게 보내는 메시지 (채널 메시지 하나는 256 바이트까지, 필드는 NUL 로 나눈다):
 *   "log\0<우선순위>\0<태그>\0<유닛>\0<메시지>"   유닛은 root 가 보낼 때만 쓰인다 (아니면 journald 가 정한다)
 *   "query\0<키>=<값>\0..." + 파이프 쓰기 끝     키: n unit tag prio pid boot follow since
 */
#include <nx64.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "nl_internal.h"
#include "nyx_abi.h"

#define JOURNAL_SVC "svc:/sys/journal"

static const char *const g_prio_names[8] = {
    "emerg", "alert", "crit", "err", "warning", "notice", "info", "debug"
};

int nx64_log_priority(const char *name)
{
    int i;

    if (name == NULL)
        return -1;
    if (name[0] >= '0' && name[0] <= '7' && name[1] == '\0')
        return name[0] - '0';
    for (i = 0; i < 8; i++)
        if (strcmp(name, g_prio_names[i]) == 0)
            return i;
    if (strcmp(name, "error") == 0)
        return NX64_LOG_ERR;
    if (strcmp(name, "warn") == 0)
        return NX64_LOG_WARNING;
    return -1;
}

const char *nx64_log_priority_name(int prio)
{
    return prio >= 0 && prio < 8 ? g_prio_names[prio] : "?";
}

/* ---------------- 쓰기 ---------------- */

static long g_jh = -1;                  /* svc:/sys/journal 송신 핸들 (close-on-exec) */

static long journal_handle(void)
{
    if (g_jh < 0) {
        g_jh = __nl_channel_open(JOURNAL_SVC);
        if (g_jh >= 0)
            (void)__nl_handle_cloexec(g_jh, 1);
    }
    return g_jh;
}

/* 메시지 하나. 큐가 차 있으면 1초까지 기다린다. journald 가 다시 시작했으면 한 번 다시 연다 */
static int journal_send_raw(const char *msg, size_t len)
{
    int reopened = 0;
    int waited = 0;

    for (;;) {
        long h = journal_handle();
        long r;

        if (h < 0)
            return -1;
        r = __nl_channel_send(h, msg, len);
        if (r >= 0)
            return 0;
        if (errno == EAGAIN && waited < 10) {
            struct __nl_pollent p;

            p.h = h;
            p.events = NL_POLL_OUT;
            p.revents = 0;
            (void)__nl_poll(&p, 1, 100);
            waited++;
            continue;
        }
        if (errno == EAGAIN)
            return -1;
        (void)__nl_close(g_jh);
        g_jh = -1;
        if (reopened)
            return -1;
        reopened = 1;
    }
}

static void put_field(char *buf, size_t *n, const char *s, size_t max)
{
    size_t i;

    for (i = 0; s != NULL && s[i] != '\0' && i < max; i++)
        buf[(*n)++] = s[i];
    buf[(*n)++] = '\0';
}

/* 태그가 없을 때: 이 프로세스의 이름 (커널이 정한 실행 파일 이름. 한 번 찾아 둔다).
 * journald 가 찾게 두면 짧게 사는 프로그램은 이미 끝나 있다 */
static const char *own_tag(void)
{
    static char name[NX64_JOURNAL_NAME_MAX];
    static int looked;
    struct __nl_procent *procs;
    unsigned long me;
    int n;
    int i;

    if (looked)
        return name;
    looked = 1;
    procs = malloc(64 * sizeof(*procs));
    if (procs == NULL)
        return name;
    me = (unsigned long)__nl_getpid();
    n = __nl_proc_list(procs, 64);
    for (i = 0; i < n && i < 64; i++) {
        if (procs[i].pid == me) {
            strncpy(name, procs[i].name, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            break;
        }
    }
    free(procs);
    return name;
}

int nx64_journal_send(int prio, const char *tag, const char *message)
{
    char msg[NX_CHAN_MSG_MAX];
    const char *p = message != NULL ? message : "";
    int result = 0;

    if (prio < 0 || prio > 7) {
        errno = EINVAL;
        return -1;
    }
    if (tag == NULL || tag[0] == '\0')
        tag = own_tag();
    do {
        size_t n = 0;
        size_t head;
        size_t len = 0;

        put_field(msg, &n, "log", 3);
        msg[n++] = (char)('0' + prio);
        msg[n++] = '\0';
        put_field(msg, &n, tag, NX64_JOURNAL_NAME_MAX - 1);
        put_field(msg, &n, "", 0);      /* 유닛: journald 가 정한다 */
        head = n;
        while (p[len] != '\0' && p[len] != '\n' && len < NX64_JOURNAL_LINE_MAX && head + len < sizeof(msg))
            len++;
        memcpy(msg + head, p, len);
        if (journal_send_raw(msg, head + len) < 0)
            return -1;
        p += len;
        if (*p == '\n')
            p++;
    } while (*p != '\0');
    return result;
}

int nx64_journal_printf(int prio, const char *fmt, ...)
{
    char text[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    return nx64_journal_send(prio, NULL, text);
}

/* ---------------- 읽기 ---------------- */

/* "key=value\0" 를 붙인다. 자리가 모자라면 -1 */
static int put_kv(char *buf, size_t *n, size_t cap, const char *key, const char *val)
{
    size_t kl = strlen(key);
    size_t vl = strlen(val);

    if (*n + kl + 1 + vl + 1 > cap)
        return -1;
    memcpy(buf + *n, key, kl);
    buf[*n + kl] = '=';
    memcpy(buf + *n + kl + 1, val, vl);
    buf[*n + kl + 1 + vl] = '\0';
    *n += kl + vl + 2;
    return 0;
}

int nx64_journal_open(const struct nx64_journal_query *q)
{
    char msg[NX_CHAN_MSG_MAX];
    char num[24];
    size_t n = 0;
    int fds[2];
    int bad = 0;
    int saved;

    memcpy(msg, "query", 6);
    n = 6;
    if (q != NULL) {
        if (q->unit != NULL && q->unit[0] != '\0')
            bad |= put_kv(msg, &n, sizeof(msg), "unit", q->unit);
        if (q->tag != NULL && q->tag[0] != '\0')
            bad |= put_kv(msg, &n, sizeof(msg), "tag", q->tag);
        if (q->max_prio >= 0) {
            sprintf(num, "%d", q->max_prio);
            bad |= put_kv(msg, &n, sizeof(msg), "prio", num);
        }
        if (q->pid != 0) {
            sprintf(num, "%lu", q->pid);
            bad |= put_kv(msg, &n, sizeof(msg), "pid", num);
        }
        if (q->lines >= 0) {
            sprintf(num, "%ld", q->lines);
            bad |= put_kv(msg, &n, sizeof(msg), "n", num);
        }
        if (q->this_boot)
            bad |= put_kv(msg, &n, sizeof(msg), "boot", "1");
        if (q->follow)
            bad |= put_kv(msg, &n, sizeof(msg), "follow", "1");
        if (q->since > 0) {
            sprintf(num, "%ld", q->since);
            bad |= put_kv(msg, &n, sizeof(msg), "since", num);
        }
    }
    if (bad) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (pipe(fds) < 0)
        return -1;
    if (nx64_service_send_fd(JOURNAL_SVC, msg, n, fds[1]) < 0) {
        saved = errno;
        close(fds[0]);
        close(fds[1]);
        errno = saved;
        return -1;
    }
    close(fds[1]);                      /* journald 가 사본을 가졌다: 그쪽이 닫으면 EOF */
    return fds[0];
}

/* 공백까지의 한 필드를 떼어 낸다 */
static char *next_field(char **p)
{
    char *s = *p;
    char *e;

    if (s == NULL)
        return NULL;
    e = strchr(s, ' ');
    if (e == NULL) {
        *p = NULL;
    } else {
        *e = '\0';
        *p = e + 1;
    }
    return s;
}

static void copy_name(char *dst, const char *src)
{
    if (strcmp(src, "-") == 0)
        src = "";
    strncpy(dst, src, NX64_JOURNAL_NAME_MAX - 1);
    dst[NX64_JOURNAL_NAME_MAX - 1] = '\0';
}

int nx64_journal_parse(char *line, struct nx64_journal_entry *e)
{
    char *p = line;
    char *f[7];
    size_t len;
    int i;

    if (line == NULL || e == NULL) {
        errno = EINVAL;
        return -1;
    }
    len = strlen(line);
    if (len > 0 && line[len - 1] == '\n')
        line[len - 1] = '\0';
    for (i = 0; i < 7; i++) {
        f[i] = next_field(&p);
        if (f[i] == NULL || f[i][0] == '\0') {
            errno = EINVAL;
            return -1;
        }
    }
    e->usec = strtol(f[0], NULL, 10);
    e->boot = strtoul(f[1], NULL, 10);
    e->pid = strtoul(f[2], NULL, 10);
    e->uid = strtoul(f[3], NULL, 10);
    e->prio = atoi(f[4]);
    copy_name(e->unit, f[5]);
    copy_name(e->tag, f[6]);
    e->message = p != NULL ? p : "";
    return 0;
}

size_t nx64_journal_format(const struct nx64_journal_entry *e, const char *host, char *buf, size_t cap)
{
    time_t t = (time_t)(e->usec / 1000000L);
    struct tm *tm = gmtime(&t);
    char when[32];
    int n;

    if (cap == 0)
        return 0;
    if (tm == NULL || strftime(when, sizeof(when), "%b %d %H:%M:%S", tm) == 0)
        strcpy(when, "??? ?? ??:??:??");
    n = snprintf(buf, cap, "%s %s %s[%lu]: %s", when, host != NULL && host[0] != '\0' ? host : "nyxis",
                 e->tag[0] != '\0' ? e->tag : (e->unit[0] != '\0' ? e->unit : "?"), e->pid, e->message);
    if (n < 0)
        return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}
