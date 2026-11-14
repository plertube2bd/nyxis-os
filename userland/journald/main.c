/*
 * journald - 시스템 저널 데몬 (init 의 유닛 journald 가 부팅 때 띄운다. 그 밖에서 띄울 일은 없다)
 *
 * 받는 것 (svc:/sys/journal, 누구나 보낼 수 있다. 커널이 메시지마다 보낸 쪽 pid/uid 를 붙인다):
 *   "log\0<우선순위>\0<태그>\0<유닛>\0<메시지>"  항목 하나 (nx64_journal_send). 유닛은 root 가 보낸 것만 믿는다.
 *                                              태그가 비면 보낸 프로세스의 이름.
 *   "stream\0<유닛>\0<pid>" + 파이프 읽기 끝     서비스의 stdout/stderr (init 만: uid 0). 줄마다 항목 하나.
 *                                              줄이 "<N>" 으로 시작하면 우선순위 N (sd-daemon 과 같다), 아니면 6(info).
 *   "query\0<키>=<값>\0..." + 파이프 쓰기 끝     읽기 (nx64_journal_open). fork 한 자식이 맞는 항목을 파이프에 쓴다.
 *                                              root 가 아니면 보낸 uid 의 항목만. follow 면 새 항목을 계속 쓴다.
 * 뜨면 init 에게 "journal-ready" 를 보낸다: init 이 서비스 출력 파이프와 모아 둔 로그를 넘긴다.
 *
 * 저장 (한 줄에 항목 하나, 형식은 nx64.h 의 nx64_journal_parse):
 *   <dir>/journal    지금 쓰는 파일 (root 0600). max_kib 를 넘으면 journal.1 로 옮기고 새로 시작한다 (journal.1 은 하나만)
 *   dir: 설정 storage (app:/journald.run/storage) = auto(기본) | persistent | volatile
 *        persistent = disk:/journal (재부팅해도 남는다), volatile = fs:/var/log/journal (재부팅하면 사라진다),
 *        auto = disk:/ 가 있으면 persistent, 없으면 volatile
 *   max_kib: 설정 max_kib (64..8192, 기본 512)
 * 부팅 번호 = 부팅 시각(UNIX 초): 실시간 시계 - 부팅 후 경과 시간. journalman -b 가 쓴다.
 */
#include <nx64.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SVC_NAME        "svc:/sys/journal"
#define STREAM_MAX      24
#define STREAM_LINE     1024
#define CHILD_MAX       8
#define NAME_MAX_       NX64_JOURNAL_NAME_MAX
#define MSG_MAX         256

struct stream {
    int fd;
    unsigned long pid;
    char unit[NAME_MAX_];
    char buf[STREAM_LINE];
    size_t len;
};

static struct stream g_streams[STREAM_MAX];
static int g_nstreams;

static char g_dir[64];
static char g_path[96];
static char g_old[96];
static int g_fd = -1;
static long g_size;
static long g_max = 512L * 1024L;
static unsigned long g_boot;

/* 읽기(query)를 하는 자식 프로세스. root 가 아닌 사용자는 CHILD_PER_USER 개까지 (follow 로 다 차지하지 못하게) */
#define CHILD_PER_USER 3
struct child {
    pid_t pid;
    unsigned long uid;
};
static struct child g_child[CHILD_MAX];
static int g_children;

/* ---------------- 시각 ---------------- */

static long now_usec(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
        return 0;
    return (long)ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}

static unsigned long boot_id(void)
{
    struct timespec rt;
    struct timespec mono;
    long diff_us;

    if (clock_gettime(CLOCK_REALTIME, &rt) < 0 || clock_gettime(CLOCK_MONOTONIC, &mono) < 0)
        return 0;
    diff_us = ((long)rt.tv_sec - (long)mono.tv_sec) * 1000000L + (rt.tv_nsec - mono.tv_nsec) / 1000L;
    return (unsigned long)((diff_us + 500000L) / 1000000L);      /* RTC 는 초 단위라 반올림하면 늘 같다 */
}

/* ---------------- 저장 ---------------- */

static int mkdir_p(const char *path, mode_t last_mode)
{
    char tmp[96];
    char *p;
    char *start;

    snprintf(tmp, sizeof(tmp), "%s", path);
    start = strstr(tmp, ":/");
    start = start != NULL ? start + 2 : tmp;
    for (p = start; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, last_mode) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int try_dir(const char *dir)
{
    snprintf(g_dir, sizeof(g_dir), "%s", dir);
    if (mkdir_p(dir, 0700) < 0)
        return -1;
    snprintf(g_path, sizeof(g_path), "%s/journal", dir);
    snprintf(g_old, sizeof(g_old), "%s/journal.1", dir);
    g_fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (g_fd < 0)
        return -1;
    {
        struct stat st;

        g_size = fstat(g_fd, &st) == 0 ? (long)st.st_size : 0;
    }
    (void)chmod(g_path, 0600);
    return 0;
}

static int open_storage(void)
{
    char mode[32];
    char kib[32];
    struct stat st;
    int have_disk = stat("disk:/", &st) == 0;

    if (nx64_setting_get("journald", "max_kib", kib, sizeof(kib)) >= 0) {
        long v = strtol(kib, NULL, 10);

        if (v >= 64 && v <= 8192)
            g_max = v * 1024L;
    }
    if (nx64_setting_get("journald", "storage", mode, sizeof(mode)) < 0)
        strcpy(mode, "auto");
    if (strcmp(mode, "persistent") == 0 || (strcmp(mode, "auto") == 0 && have_disk)) {
        if (try_dir("disk:/journal") == 0)
            return 0;
        if (strcmp(mode, "persistent") == 0)       /* auto 면 조용히 volatile 로 */
            fprintf(stderr, "journald: cannot use disk:/journal (%s), falling back to volatile storage\n",
                    strerror(errno));
    }
    return try_dir("fs:/var/log/journal");
}

static void rotate(void)
{
    close(g_fd);
    (void)unlink(g_old);
    (void)rename(g_path, g_old);
    g_fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    g_size = 0;
}

/* 이름 (유닛/태그): 보이는 ASCII 만, 공백 없이. 비면 "-" */
static void clean_name(char *dst, const char *src)
{
    size_t n = 0;

    for (; src != NULL && *src != '\0' && n + 1 < NAME_MAX_; src++)
        if ((unsigned char)*src > ' ' && (unsigned char)*src < 0x7f)
            dst[n++] = *src;
    if (n == 0)
        dst[n++] = '-';
    dst[n] = '\0';
}

/* 항목 하나를 쓴다. msg 는 개행이 없는 한 줄 (len 바이트) */
static void write_record(unsigned long pid, unsigned long uid, int prio, const char *unit, const char *tag,
                         const char *msg, size_t len)
{
    char line[STREAM_LINE + 160];
    char u[NAME_MAX_];
    char t[NAME_MAX_];
    int head;
    size_t n;
    size_t i;

    if (g_fd < 0)
        return;
    while (len > 0 && (msg[len - 1] == '\r' || msg[len - 1] == ' '))
        len--;
    clean_name(u, unit);
    clean_name(t, tag);
    head = snprintf(line, sizeof(line), "%ld %lu %lu %lu %d %s %s ", now_usec(), g_boot, pid, uid,
                    prio < 0 || prio > 7 ? NX64_LOG_INFO : prio, u, t);
    if (head < 0)
        return;
    n = (size_t)head;
    for (i = 0; i < len && n + 2 < sizeof(line); i++) {
        unsigned char c = (unsigned char)msg[i];

        if (c == '\t')
            c = ' ';
        else if (c < 0x20 || c == 0x7f)
            c = '?';                    /* 제어 문자(터미널 이스케이프 등)는 저장하지 않는다 */
        line[n++] = (char)c;
    }
    line[n++] = '\n';
    if (g_size + (long)n > g_max && g_size > 0)
        rotate();
    if (g_fd >= 0 && write(g_fd, line, n) == (ssize_t)n)
        g_size += (long)n;
}

/* journald 자신의 항목 */
static void jlog(int prio, const char *fmt, ...)
{
    char text[STREAM_LINE];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    write_record((unsigned long)getpid(), 0, prio, "journald", "journald", text, strlen(text));
}

/* 여러 줄 메시지를 줄마다 */
static void write_lines(unsigned long pid, unsigned long uid, int prio, const char *unit, const char *tag,
                        const char *msg, size_t len)
{
    size_t start = 0;
    size_t i;

    for (i = 0; i <= len; i++) {
        if (i == len || msg[i] == '\n') {
            if (i > start || len == 0)
                write_record(pid, uid, prio, unit, tag, msg + start, i - start);
            start = i + 1;
        }
    }
}

/* ---------------- 보낸 쪽 ---------------- */

/* pid 의 프로세스 이름 (태그가 비었을 때) */
static void process_name(unsigned long pid, char *out)
{
    static struct nx64_proc procs[128];
    static unsigned long cached_pid;
    static char cached_name[NAME_MAX_];
    int n;
    int i;

    if (pid == cached_pid && cached_pid != 0) {
        strcpy(out, cached_name);
        return;
    }
    strcpy(out, "-");
    n = nx64_processes(procs, 128);
    for (i = 0; i < n && i < 128; i++) {
        if (procs[i].pid == pid) {
            clean_name(out, procs[i].name);
            cached_pid = pid;
            strcpy(cached_name, out);
            return;
        }
    }
}

/* pid 가 어느 유닛의 서비스인지 (stream 으로 알게 된 것) */
static const char *unit_of_pid(unsigned long pid)
{
    int i;

    for (i = 0; i < g_nstreams; i++)
        if (g_streams[i].pid == pid)
            return g_streams[i].unit;
    return "";
}

/* NUL 로 나뉜 필드를 꺼낸다. 마지막 필드(메시지)는 *rest 로 */
static int split_fields(char *msg, size_t len, const char **f, int nf, const char **rest, size_t *rest_len)
{
    size_t pos = 0;
    int k;

    for (k = 0; k < nf; k++) {
        size_t s = pos;

        while (pos < len && msg[pos] != '\0')
            pos++;
        if (pos >= len)
            return -1;
        f[k] = msg + s;
        pos++;
    }
    *rest = msg + pos;
    *rest_len = len - pos;
    return 0;
}

static void handle_log(char *msg, size_t len, const struct nx64_sender *from)
{
    const char *f[4];
    const char *text;
    size_t tlen;
    char tag[NAME_MAX_];
    const char *unit;
    int prio;

    if (split_fields(msg, len, f, 4, &text, &tlen) < 0)
        return;
    prio = (f[1][0] >= '0' && f[1][0] <= '7' && f[1][1] == '\0') ? f[1][0] - '0' : NX64_LOG_INFO;
    if (f[2][0] != '\0')
        clean_name(tag, f[2]);
    else
        process_name(from->pid, tag);
    unit = from->uid == 0 && f[3][0] != '\0' ? f[3] : unit_of_pid(from->pid);
    write_lines(from->pid, from->uid, prio, unit, tag, text, tlen);
}

/* ---------------- 서비스 출력 (stream) ---------------- */

static void stream_line(struct stream *s, char *line, size_t len)
{
    int prio = NX64_LOG_INFO;

    if (len >= 3 && line[0] == '<' && line[1] >= '0' && line[1] <= '7' && line[2] == '>') {
        prio = line[1] - '0';
        line += 3;
        len -= 3;
    }
    write_record(s->pid, 0, prio, s->unit, s->unit, line, len);
}

static void stream_close(int i)
{
    struct stream *s = &g_streams[i];

    if (s->len > 0)
        stream_line(s, s->buf, s->len);
    close(s->fd);
    g_streams[i] = g_streams[--g_nstreams];
}

static void handle_stream(char *msg, size_t len, const struct nx64_sender *from, int fd)
{
    const char *f[2];
    const char *pid;
    size_t pid_len;
    struct stream *s;
    char num[24];

    if (from->uid != 0 || fd < 0 || split_fields(msg, len, f, 2, &pid, &pid_len) < 0) {
        if (fd >= 0)
            close(fd);
        return;
    }
    if (g_nstreams >= STREAM_MAX) {
        jlog(NX64_LOG_ERR, "too many service streams: output of %s is lost", f[1]);
        close(fd);
        return;
    }
    s = &g_streams[g_nstreams++];
    s->fd = fd;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    clean_name(s->unit, f[1]);
    snprintf(num, sizeof(num), "%.*s", (int)(pid_len < 20 ? pid_len : 20), pid);
    s->pid = strtoul(num, NULL, 10);
    s->len = 0;
}

/* 읽을 것이 있다. 끝(EOF)이면 0 */
static int stream_read(struct stream *s)
{
    ssize_t n = read(s->fd, s->buf + s->len, sizeof(s->buf) - s->len);
    size_t start = 0;
    size_t i;

    if (n <= 0)
        return 0;
    s->len += (size_t)n;
    for (i = 0; i < s->len; i++) {
        if (s->buf[i] == '\n') {
            stream_line(s, s->buf + start, i - start);
            start = i + 1;
        }
    }
    if (start == 0 && s->len == sizeof(s->buf)) {    /* 너무 긴 줄: 그대로 자른다 */
        stream_line(s, s->buf, s->len);
        start = s->len;
    }
    memmove(s->buf, s->buf + start, s->len - start);
    s->len -= start;
    return 1;
}

/* ---------------- 읽기 (query) ---------------- */

struct query {
    char unit[NAME_MAX_];
    char tag[NAME_MAX_];
    int max_prio;
    unsigned long pid;
    long lines;
    int this_boot;
    int follow;
    long since_us;
    unsigned long uid;                  /* 묻는 사람 */
};

static int write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);

        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int matches(const struct query *q, const char *line, size_t len)
{
    char tmp[STREAM_LINE + 160];
    struct nx64_journal_entry e;

    if (len >= sizeof(tmp))
        len = sizeof(tmp) - 1;
    memcpy(tmp, line, len);
    tmp[len] = '\0';
    if (nx64_journal_parse(tmp, &e) < 0)
        return 0;
    if (q->uid != 0 && e.uid != q->uid)
        return 0;
    if (q->unit[0] != '\0' && strcmp(e.unit, q->unit) != 0)
        return 0;
    if (q->tag[0] != '\0' && strcmp(e.tag, q->tag) != 0)
        return 0;
    if (q->max_prio >= 0 && e.prio > q->max_prio)
        return 0;
    if (q->pid != 0 && e.pid != q->pid)
        return 0;
    if (q->this_boot && e.boot != g_boot)
        return 0;
    if (q->since_us > 0 && e.usec < q->since_us)
        return 0;
    return 1;
}

/* 파일 하나를 통째로 읽는다 (없으면 빈 것) */
static char *slurp(const char *path, long *len)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    char *buf;
    long n = 0;

    *len = 0;
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) < 0 || (buf = malloc((size_t)st.st_size + 1)) == NULL) {
        close(fd);
        return NULL;
    }
    while (n < (long)st.st_size) {
        ssize_t r = read(fd, buf + n, (size_t)st.st_size - (size_t)n);

        if (r <= 0)
            break;
        n += r;
    }
    close(fd);
    *len = n;
    return buf;
}

struct match {
    const char *p;
    size_t len;
};

static void scan(const struct query *q, const char *buf, long len, struct match **m, size_t *nm, size_t *cap)
{
    long start = 0;
    long i;

    for (i = 0; i < len; i++) {
        if (buf[i] != '\n')
            continue;
        if (matches(q, buf + start, (size_t)(i - start))) {
            if (*nm == *cap) {
                size_t nc = *cap ? *cap * 2 : 256;
                struct match *nmv = realloc(*m, nc * sizeof(**m));

                if (nmv == NULL)
                    return;
                *m = nmv;
                *cap = nc;
            }
            (*m)[*nm].p = buf + start;
            (*m)[*nm].len = (size_t)(i - start + 1);
            (*nm)++;
        }
        start = i + 1;
    }
}

/* follow: 지금 파일이 자라면 새 줄을 보낸다. 받는 쪽이 닫으면 끝 */
static void follow(int out, const struct query *q, long offset)
{
    char buf[4096];
    size_t have = 0;

    for (;;) {
        struct pollfd p;
        struct stat st;
        int fd;

        p.fd = out;
        p.events = 0;
        p.revents = 0;
        if (poll(&p, 1, 300) > 0 && (p.revents & (POLLHUP | POLLERR)))
            return;
        if (stat(g_path, &st) < 0)
            continue;
        if ((long)st.st_size < offset) {          /* journal.1 로 옮겨지고 새 파일이 시작됐다 */
            offset = 0;
            have = 0;
        }
        if ((long)st.st_size == offset)
            continue;
        fd = open(g_path, O_RDONLY);
        if (fd < 0)
            continue;
        if (lseek(fd, offset, SEEK_SET) == offset) {
            ssize_t r;

            while ((r = read(fd, buf + have, sizeof(buf) - have)) > 0) {
                size_t start = 0;
                size_t i;

                offset += r;
                have += (size_t)r;
                for (i = 0; i < have; i++) {
                    if (buf[i] != '\n')
                        continue;
                    if (matches(q, buf + start, i - start) && write_all(out, buf + start, i - start + 1) < 0) {
                        close(fd);
                        return;
                    }
                    start = i + 1;
                }
                if (start == 0 && have == sizeof(buf))
                    start = have;               /* 줄이 너무 길다: 버린다 */
                memmove(buf, buf + start, have - start);
                have -= start;
            }
        }
        close(fd);
    }
}

static void run_query(int out, const struct query *q)
{
    long old_len;
    long cur_len;
    char *old_buf = slurp(g_old, &old_len);
    char *cur_buf = slurp(g_path, &cur_len);
    struct match *m = NULL;
    size_t nm = 0;
    size_t cap = 0;
    size_t first = 0;
    size_t i;

    if (old_buf != NULL)
        scan(q, old_buf, old_len, &m, &nm, &cap);
    if (cur_buf != NULL)
        scan(q, cur_buf, cur_len, &m, &nm, &cap);
    if (q->lines >= 0 && nm > (size_t)q->lines)
        first = nm - (size_t)q->lines;
    for (i = first; i < nm; i++)
        if (write_all(out, m[i].p, m[i].len) < 0)
            return;
    if (q->follow) {
        /* 마지막 완성된 줄 뒤부터 */
        long off = cur_len;

        while (off > 0 && cur_buf[off - 1] != '\n')
            off--;
        follow(out, q, off);
    }
}

static void handle_query(char *msg, size_t len, const struct nx64_sender *from, int fd)
{
    struct query q;
    size_t pos = 6;                     /* "query\0" 다음 */
    pid_t pid;
    int i;

    if (fd < 0)
        return;
    memset(&q, 0, sizeof(q));
    q.max_prio = -1;
    q.lines = -1;
    q.uid = from->uid;
    while (pos < len) {
        char kv[MSG_MAX];
        size_t n = 0;
        char *eq;

        while (pos < len && msg[pos] != '\0' && n + 1 < sizeof(kv))
            kv[n++] = msg[pos++];
        kv[n] = '\0';
        pos++;
        eq = strchr(kv, '=');
        if (eq == NULL)
            continue;
        *eq++ = '\0';
        if (strcmp(kv, "unit") == 0)
            clean_name(q.unit, eq);
        else if (strcmp(kv, "tag") == 0)
            clean_name(q.tag, eq);
        else if (strcmp(kv, "prio") == 0)
            q.max_prio = atoi(eq);
        else if (strcmp(kv, "pid") == 0)
            q.pid = strtoul(eq, NULL, 10);
        else if (strcmp(kv, "n") == 0)
            q.lines = strtol(eq, NULL, 10);
        else if (strcmp(kv, "boot") == 0)
            q.this_boot = atoi(eq) != 0;
        else if (strcmp(kv, "follow") == 0)
            q.follow = atoi(eq) != 0;
        else if (strcmp(kv, "since") == 0)
            q.since_us = strtol(eq, NULL, 10) * 1000000L;
    }

    {
        int mine = 0;

        for (i = 0; i < g_children; i++)
            if (g_child[i].uid == from->uid)
                mine++;
        if (g_children >= CHILD_MAX || (from->uid != 0 && mine >= CHILD_PER_USER)) {
            char busy[96];
            int n = snprintf(busy, sizeof(busy), "%ld %lu 0 %lu 3 journald journald too many readers, try again later\n",
                             now_usec(), g_boot, from->uid);

            (void)write(fd, busy, (size_t)n);
            close(fd);
            return;
        }
    }
    pid = fork();
    if (pid == 0) {
        for (i = 0; i < g_nstreams; i++)
            close(g_streams[i].fd);
        if (g_fd >= 0)
            close(g_fd);
        run_query(fd, &q);
        _exit(0);
    }
    close(fd);
    if (pid > 0) {
        g_child[g_children].pid = pid;
        g_child[g_children].uid = from->uid;
        g_children++;
    }
}

/* 끝난 자식을 거둔다 */
static void reap(void)
{
    pid_t pid;
    int st;

    while (g_children > 0 && (pid = waitpid(-1, &st, WNOHANG)) > 0) {
        int i;

        for (i = 0; i < g_children; i++) {
            if (g_child[i].pid == pid) {
                g_child[i] = g_child[--g_children];
                break;
            }
        }
    }
}

/* ---------------- 메인 ---------------- */

static void handle_message(long h)
{
    int k;

    for (k = 0; k < 32; k++) {           /* 한 번에 너무 오래 붙잡지 않게 */
        char msg[MSG_MAX + 1];
        struct nx64_sender from;
        int fd = -1;
        long len = nx64_service_receive_fd(h, msg, MSG_MAX, &from, 0, &fd);

        if (len < 0)
            return;
        msg[len] = '\0';
        if (len >= 4 && memcmp(msg, "log", 4) == 0) {
            handle_log(msg, (size_t)len, &from);
            if (fd >= 0)
                close(fd);
        } else if (len >= 7 && memcmp(msg, "stream", 7) == 0) {
            handle_stream(msg, (size_t)len, &from, fd);
        } else if (len >= 6 && memcmp(msg, "query", 6) == 0) {
            handle_query(msg, (size_t)len, &from, fd);
        } else if (fd >= 0) {
            close(fd);
        }
    }
}

int main(int argc, char **argv)
{
    long h;
    int cfd;

    (void)argc;
    (void)argv;
    if (getuid() != 0) {
        fprintf(stderr, "journald: must run as root (it is started by init: nex status journald)\n");
        return 1;
    }
    if (open_storage() < 0) {
        fprintf(stderr, "journald: cannot open journal in %s: %s\n", g_dir, strerror(errno));
        return 1;
    }
    g_boot = boot_id();
    h = nx64_service_create(SVC_NAME, 1);
    if (h < 0) {
        fprintf(stderr, "journald: cannot create %s: %s\n", SVC_NAME, strerror(errno));
        return 1;
    }
    cfd = nx64_service_fd(h);
    jlog(NX64_LOG_INFO, "Journal started: %s (max %ld KiB), boot %lu", g_path, g_max / 1024L, g_boot);
    (void)nx64_service_send("svc:/sys/init", "journal-ready", 13);

    for (;;) {
        struct pollfd fds[1 + STREAM_MAX];
        int n = 0;
        int i;

        fds[n].fd = cfd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
        for (i = 0; i < g_nstreams; i++) {
            fds[n].fd = g_streams[i].fd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            n++;
        }
        /* 자식(읽기)이 있으면 가끔 깨어 거둔다 */
        if (poll(fds, (nfds_t)n, g_children > 0 ? 500 : -1) > 0) {
            if (fds[0].revents & POLLIN)
                handle_message(h);
            /* 뒤에서부터: stream_close 가 마지막 것을 i 로 옮긴다 */
            for (i = n - 1; i >= 1; i--) {
                int si = i - 1;

                if (si >= g_nstreams || g_streams[si].fd != fds[i].fd)
                    continue;
                if (fds[i].revents & (POLLIN | POLLHUP)) {
                    if (!stream_read(&g_streams[si]))
                        stream_close(si);
                }
            }
        }
        reap();
    }
}
