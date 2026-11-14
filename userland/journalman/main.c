/*
 * journalman - 저널 보기/쓰기 (journalctl + logger 와 비슷하다). 저널은 journald 가 모은다
 *
 *   journalman [옵션]                       항목 보기 (오래된 것부터)
 *     -u, --unit 유닛       그 유닛(nex 서비스)의 것만 (서비스 출력 + init 의 시작/멈춤 기록)
 *     -t, --tag 태그        그 태그(프로그램 이름)만
 *     -p, --priority 우선   그 우선순위와 더 중요한 것만: emerg alert crit err warning notice info debug 또는 0..7
 *     -n, --lines N         마지막 N 개 (-f 일 때 기본 10)
 *     -f, --follow          새 항목을 계속 보인다 (Ctrl-C 로 끝)
 *     -b, --boot            이번 부팅의 것만
 *         --pid PID         그 프로세스의 것만
 *     -S, --since 때        30s 10m 2h 1d 전부터, 또는 @UNIX초
 *     -o, --output 형식     short(기본) | cat(메시지만) | verbose(모든 필드) | raw(저장된 줄 그대로)
 *         --no-color
 *   journalman log [-p 우선] [-t 태그] [메시지...]   항목 쓰기 (메시지가 없으면 stdin 의 줄마다)
 *
 * root 는 모든 항목을, 다른 사용자는 자기 uid 의 항목만 본다 (시스템 로그는 elev journalman).
 */
#include <nx64.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { OUT_SHORT, OUT_CAT, OUT_VERBOSE, OUT_RAW };

static int g_color;

static void usage(FILE *out)
{
    fprintf(out,
            "usage: journalman [options]            show the journal\n"
            "       journalman log [-p prio] [-t tag] [message...]\n"
            "options:\n"
            "  -u, --unit UNIT        only this nex unit\n"
            "  -t, --tag TAG          only this tag (program name)\n"
            "  -p, --priority PRIO    this priority and more important (err, warning, info, 0..7)\n"
            "  -n, --lines N          last N entries\n"
            "  -f, --follow           keep showing new entries\n"
            "  -b, --boot             this boot only\n"
            "      --pid PID          only this process\n"
            "  -S, --since TIME       30s, 10m, 2h, 1d ago, or @unix-seconds\n"
            "  -o, --output FORMAT    short | cat | verbose | raw\n"
            "      --no-color\n");
}

static int parse_since(const char *s, long *out)
{
    char *end;
    long v;
    long mult = 1;

    if (s[0] == '@') {
        v = strtol(s + 1, &end, 10);
        if (*end != '\0' || v <= 0)
            return -1;
        *out = v;
        return 0;
    }
    v = strtol(s, &end, 10);
    if (end == s || v < 0)
        return -1;
    if (*end == '\0' || strcmp(end, "s") == 0)
        mult = 1;
    else if (strcmp(end, "m") == 0 || strcmp(end, "min") == 0)
        mult = 60;
    else if (strcmp(end, "h") == 0)
        mult = 3600;
    else if (strcmp(end, "d") == 0)
        mult = 86400;
    else
        return -1;
    *out = (long)time(NULL) - v * mult;
    return *out > 0 ? 0 : -1;
}

static const char *prio_color(int prio)
{
    if (!g_color)
        return "";
    if (prio <= NX64_LOG_ERR)
        return "\033[1;31m";
    if (prio == NX64_LOG_WARNING)
        return "\033[33m";
    if (prio == NX64_LOG_NOTICE)
        return "\033[1m";
    if (prio == NX64_LOG_DEBUG)
        return "\033[90m";
    return "";
}

static void print_entry(char *line, int mode, const char *host)
{
    struct nx64_journal_entry e;
    const char *on;
    const char *off;

    if (mode == OUT_RAW) {
        fputs(line, stdout);
        return;
    }
    if (nx64_journal_parse(line, &e) < 0)
        return;
    on = prio_color(e.prio);
    off = on[0] != '\0' ? "\033[0m" : "";
    if (mode == OUT_CAT) {
        printf("%s%s%s\n", on, e.message, off);
    } else if (mode == OUT_VERBOSE) {
        time_t t = (time_t)(e.usec / 1000000L);
        struct tm *tm = gmtime(&t);
        char when[48];

        if (tm == NULL || strftime(when, sizeof(when), "%a %Y-%m-%d %H:%M:%S", tm) == 0)
            strcpy(when, "?");
        printf("%s.%06ld UTC [boot %lu]\n", when, e.usec % 1000000L, e.boot);
        printf("    PRIORITY=%d (%s)\n", e.prio, nx64_log_priority_name(e.prio));
        if (e.unit[0] != '\0')
            printf("    UNIT=%s\n", e.unit);
        if (e.tag[0] != '\0')
            printf("    TAG=%s\n", e.tag);
        printf("    PID=%lu\n    UID=%lu\n", e.pid, e.uid);
        printf("    MESSAGE=%s%s%s\n", on, e.message, off);
    } else {
        char out[1200];

        nx64_journal_format(&e, host, out, sizeof(out));
        printf("%s%s%s\n", on, out, off);
    }
}

static int cmd_show(int argc, char **argv)
{
    struct nx64_journal_query q;
    char host[64];
    int mode = OUT_SHORT;
    int lines_given = 0;
    FILE *f;
    char *line = NULL;
    size_t cap = 0;
    int fd;
    int i;
    int count = 0;

    memset(&q, 0, sizeof(q));
    q.max_prio = -1;
    q.lines = -1;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        int takes = 1;

        if (strcmp(a, "-u") == 0 || strcmp(a, "--unit") == 0) {
            q.unit = v;
        } else if (strcmp(a, "-t") == 0 || strcmp(a, "--tag") == 0) {
            q.tag = v;
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--priority") == 0) {
            if (v == NULL || (q.max_prio = nx64_log_priority(v)) < 0) {
                fprintf(stderr, "journalman: bad priority: %s (emerg alert crit err warning notice info debug)\n",
                        v ? v : "");
                return 2;
            }
        } else if (strcmp(a, "-n") == 0 || strcmp(a, "--lines") == 0) {
            char *end;

            if (v == NULL || (q.lines = strtol(v, &end, 10)) < 0 || *end != '\0') {
                fprintf(stderr, "journalman: bad number of lines: %s\n", v ? v : "");
                return 2;
            }
            lines_given = 1;
        } else if (strcmp(a, "--pid") == 0) {
            if (v == NULL || (q.pid = strtoul(v, NULL, 10)) == 0) {
                fprintf(stderr, "journalman: bad pid: %s\n", v ? v : "");
                return 2;
            }
        } else if (strcmp(a, "-S") == 0 || strcmp(a, "--since") == 0) {
            if (v == NULL || parse_since(v, &q.since) < 0) {
                fprintf(stderr, "journalman: bad time: %s (30s, 10m, 2h, 1d, @unix-seconds)\n", v ? v : "");
                return 2;
            }
        } else if (strcmp(a, "-o") == 0 || strcmp(a, "--output") == 0) {
            if (v != NULL && strcmp(v, "short") == 0)
                mode = OUT_SHORT;
            else if (v != NULL && strcmp(v, "cat") == 0)
                mode = OUT_CAT;
            else if (v != NULL && strcmp(v, "verbose") == 0)
                mode = OUT_VERBOSE;
            else if (v != NULL && strcmp(v, "raw") == 0)
                mode = OUT_RAW;
            else {
                fprintf(stderr, "journalman: bad output format: %s (short cat verbose raw)\n", v ? v : "");
                return 2;
            }
        } else {
            takes = 0;
            if (strcmp(a, "-f") == 0 || strcmp(a, "--follow") == 0) {
                q.follow = 1;
            } else if (strcmp(a, "-b") == 0 || strcmp(a, "--boot") == 0) {
                q.this_boot = 1;
            } else if (strcmp(a, "--no-color") == 0) {
                g_color = 0;
            } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
                usage(stdout);
                return 0;
            } else {
                fprintf(stderr, "journalman: unknown option: %s\n", a);
                usage(stderr);
                return 2;
            }
        }
        if (takes) {
            if (v == NULL) {
                fprintf(stderr, "journalman: %s needs a value\n", a);
                return 2;
            }
            i++;
        }
    }
    if (q.follow && !lines_given)
        q.lines = 10;

    fd = nx64_journal_open(&q);
    if (fd < 0) {
        if (errno == ENOENT || errno == EPIPE)
            fprintf(stderr, "journalman: journald is not running (nex status journald)\n");
        else
            fprintf(stderr, "journalman: cannot open the journal: %s\n", strerror(errno));
        return 1;
    }
    if (nx64_info("hostname", host, sizeof(host)) < 0)
        strcpy(host, "nyxis");
    f = fdopen(fd, "r");
    if (f == NULL) {
        close(fd);
        return 1;
    }
    while (getline(&line, &cap, f) > 0) {
        print_entry(line, mode, host);
        count++;
        if (q.follow)
            fflush(stdout);
    }
    free(line);
    fclose(f);
    if (count == 0 && !q.follow)
        printf("-- No entries --\n");
    if (getuid() != 0)
        fprintf(stderr, "(you see only your own entries; system logs: elev journalman ...)\n");
    return 0;
}

static int cmd_log(int argc, char **argv)
{
    int prio = NX64_LOG_INFO;
    const char *tag = NULL;
    char text[1024];
    size_t n = 0;
    int i;

    for (i = 2; i < argc; i++) {
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--priority") == 0) && i + 1 < argc) {
            prio = nx64_log_priority(argv[++i]);
            if (prio < 0) {
                fprintf(stderr, "journalman: bad priority: %s\n", argv[i]);
                return 2;
            }
        } else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--tag") == 0) && i + 1 < argc) {
            tag = argv[++i];
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else {
            break;
        }
    }
    if (tag == NULL) {                   /* logger 처럼: 태그가 없으면 사용자 이름 */
        static struct nx64_identity me;

        if (nx64_whoami(&me) == 0 && me.name[0] != '\0')
            tag = me.name;
    }
    if (i < argc) {
        text[0] = '\0';
        for (; i < argc; i++) {
            size_t l = strlen(argv[i]);

            if (n + l + 2 >= sizeof(text))
                break;
            if (n > 0)
                text[n++] = ' ';
            memcpy(text + n, argv[i], l + 1);
            n += l;
        }
        if (nx64_journal_send(prio, tag, text) < 0) {
            fprintf(stderr, "journalman: cannot write to the journal: %s\n", strerror(errno));
            return 1;
        }
        return 0;
    }
    while (fgets(text, sizeof(text), stdin) != NULL) {
        size_t l = strlen(text);

        if (l > 0 && text[l - 1] == '\n')
            text[--l] = '\0';
        if (nx64_journal_send(prio, tag, text) < 0) {
            fprintf(stderr, "journalman: cannot write to the journal: %s\n", strerror(errno));
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    g_color = isatty(1);
    if (argc > 1 && strcmp(argv[1], "log") == 0)
        return cmd_log(argc, argv);
    return cmd_show(argc, argv);
}
