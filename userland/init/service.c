/*
 * service.c - init 의 서비스 관리자 (systemd 의 유닛과 같은 일을 작게)
 *
 * 유닛 = 백그라운드로 도는 프로그램 하나. 유닛 파일 하나가 유닛 하나이고 파일 이름이 유닛 이름이다:
 *   app:/init.run/units/<이름>        기본 유닛 (rootfs/app/init/units/, root 소유 0644)
 *   roi:/app/0/init/units/<이름>      관리자가 더하거나 덮어쓰는 유닛 (같은 이름이면 이쪽이 이긴다)
 *   roi:/app/0/init/enabled/<이름>    "yes"/"no": 부팅 때 시작할지 (nex enable/disable 이 쓴다. 없으면 유닛의 boot)
 *
 * 유닛 파일 형식 (한 줄에 "키 = 값", '#' 로 시작하는 줄은 주석):
 *   description = 설명 한 줄
 *   exec        = 실행 파일 (app:/journald.run 처럼 이름공간 경로). 꼭 있어야 한다
 *   args        = 인자 (공백으로 나눈다. "..." 로 묶으면 공백을 넣을 수 있다. 최대 15 개)
 *   restart     = no | on-failure | always      끝났을 때 다시 시작할지 (기본 no)
 *   restart_ms  = 다시 시작하기 전에 기다릴 시간 (기본 1000)
 *   stop_ms     = 멈출 때 SIGTERM 뒤 SIGKILL 까지 기다릴 시간 (기본 3000)
 *   output      = journal | console              stdout/stderr 를 저널로 (기본) 또는 콘솔로
 *   boot        = yes | no                       부팅 때 시작 (기본 no)
 *   order       = 0..99                          부팅 때 시작 순서 (작은 것 먼저, 기본 50)
 *
 * 서비스 프로세스: init 이 fork 한 뒤 exec 한다. root, 새 프로세스 그룹(콘솔의 Ctrl-C 가 닿지 않는다),
 * stdin = 빈 파이프(바로 EOF), stdout/stderr = 파이프 (output = journal) 또는 콘솔. 환경은 USER/LOGNAME/SHELL 과
 * NEX_UNIT=<유닛 이름>. 출력 파이프의 읽기 끝은 journald 에 넘긴다 (svc:/sys/journal 에 "stream" 메시지 + 핸들).
 * init 도 읽기 끝의 사본을 쥐고 있다가 journald 가 다시 시작하면("journal-ready") 새 journald 에 다시 넘긴다.
 *
 * 다시 시작: restart 정책대로. 10초 안에 5번 넘게 다시 시작하면 멈추고 failed (start-limit-hit).
 *
 * nex (svc:/sys/init 요청): "nex\0<명령>\0<유닛>\0" + 답장 채널의 송신 핸들을 붙인다 (NxChannelSendHandle).
 *   답장: 'T' + 글 (여러 번, 이어 붙이면 된다), 마지막에 'E' + 10진수 결과 (아래 NEX_*). 그 뒤 init 이 핸들을 닫는다.
 *   list            한 줄에 유닛 하나: 이름 \t 상태 \t pid \t enabled|disabled \t 설명
 *   status <유닛>   한 줄에 "키 \t 값"
 *   start/stop/restart <유닛>, enable/disable <유닛>, reload   (uid 0 만: 다른 사용자는 elev nex ...)
 *   stop/restart 는 프로세스가 실제로 끝난 뒤에 답한다.
 *
 * 저널 (ilog): init 의 일("Started journald.") 도 저널에 쓴다. journald 가 "journal-ready" 를 보내기 전에는 모아 둔다.
 */
#include "service.h"

#define UNIT_MAX        16
#define UNIT_NAME_MAX   32
#define UNIT_FILE_MAX   2048UL
#define UNIT_ARGV_MAX   16
#define UNITS_DEFAULT   "app:/init.run/units"
#define UNITS_ADMIN     "roi:/app/0/init/units"
#define ENABLED_DIR     "roi:/app/0/init/enabled"
#define JOURNAL_SVC     "svc:/sys/journal"

#define BURST_WINDOW_MS 10000UL
#define BURST_MAX       5U

/* nex 결과 (nex 의 종료 코드와 같다) */
#define NEX_OK          0
#define NEX_FAIL        1
#define NEX_NOT_FOUND   2
#define NEX_PERM        3
#define NEX_BUSY        4
#define NEX_USAGE       5

enum { ST_INACTIVE, ST_ACTIVE, ST_STOPPING, ST_FAILED, ST_RESTARTING };
enum { RS_NO, RS_ON_FAILURE, RS_ALWAYS };
enum { OUT_JOURNAL, OUT_CONSOLE };

static const char *const g_state_names[] = { "inactive", "active", "stopping", "failed", "restarting" };
static const char *const g_restart_names[] = { "no", "on-failure", "always" };
static const char *const g_output_names[] = { "journal", "console" };

struct unit_cfg {
    char source[96];
    char desc[96];
    char exec[128];
    char args[256];
    int restart;
    unsigned long restart_ms;
    unsigned long stop_ms;
    int output;
    int boot;
    int order;
};

struct unit {
    int used;
    int found;                           /* 다시 읽기(reload) 중: 유닛 파일이 아직 있다 */
    char name[UNIT_NAME_MAX];
    struct unit_cfg cfg;
    /* 실행 상태 */
    int state;
    long proc;                           /* 프로세스 핸들 (-1) */
    unsigned long pid;
    long out_r;                          /* 출력 파이프의 읽기 끝 (init 의 사본, cloexec. -1) */
    int stream_sent;                     /* 지금의 journald 에 넘겼다 */
    unsigned long since;                 /* 상태가 바뀐 때 (부팅 후 ms) */
    unsigned long deadline;              /* RESTARTING: 다시 시작할 때, STOPPING: 강제로 끝낼 때 */
    int killed;                          /* STOPPING 에서 이미 SIGKILL 을 보냈다 */
    int has_code;
    int code;                            /* 마지막 종료 코드 */
    char result[24];                     /* 마지막으로 끝난 까닭 */
    unsigned int restarts;               /* 자동으로 다시 시작한 횟수 */
    unsigned long burst_start;
    unsigned int burst;
    int then_start;                      /* 멈춘 뒤 다시 시작한다 (restart) */
    long waiter;                         /* 일이 끝나면 답할 nex 답장 채널 (-1) */
};

static struct unit g_units[UNIT_MAX];

/* ---------------- 작은 도구 ---------------- */

void set_cloexec(long h)
{
    if (h >= 0)
        (void)nx_syscall(NX_SYS_HANDLE_FLAGS, (unsigned long)h, NX_HFLAG_CLOEXEC, 0, 0, 0);
}

static unsigned long now_ms(void)
{
    long ns = nx_syscall(NX_SYS_GET_TIME, NX_CLOCK_MONOTONIC, 0, 0, 0, 0);

    return ns < 0 ? 0 : (unsigned long)ns / 1000000UL;
}

/* 잘라서 복사 (언제나 NUL 로 끝난다) */
static void scopy(char *dst, unsigned long cap, const char *src)
{
    unsigned long i = 0;

    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

struct sbuf {
    char *p;
    unsigned long n;
    unsigned long cap;
};

static void sb_init(struct sbuf *b, char *p, unsigned long cap)
{
    b->p = p;
    b->n = 0;
    b->cap = cap;
    p[0] = '\0';
}

static void sb_str(struct sbuf *b, const char *s)
{
    while (s != 0 && *s != '\0' && b->n + 1 < b->cap)
        b->p[b->n++] = *s++;
    b->p[b->n] = '\0';
}

static void sb_num(struct sbuf *b, long v)
{
    char tmp[24];
    int k = 0;
    unsigned long u = v < 0 ? (unsigned long)(-(v + 1)) + 1UL : (unsigned long)v;

    if (v < 0)
        sb_str(b, "-");
    do {
        tmp[k++] = (char)('0' + (int)(u % 10UL));
        u /= 10UL;
    } while (u != 0);
    while (k > 0 && b->n + 1 < b->cap)
        b->p[b->n++] = tmp[--k];
    b->p[b->n] = '\0';
}

/* 값 끝에 NUL 하나를 넣어 (메시지 필드 구분자) */
static void sb_nul(struct sbuf *b)
{
    if (b->n + 1 < b->cap)
        b->n++;                          /* p[n] 은 이미 NUL: 그것을 필드 끝으로 쓰고 새 NUL 을 둔다 */
    b->p[b->n] = '\0';
}

static int valid_name(const char *s)
{
    unsigned long n = 0;

    if (s[0] == '\0' || s[0] == '.')
        return 0;
    for (; s[n] != '\0'; n++) {
        char c = s[n];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
            return 0;
        if (n + 1 >= UNIT_NAME_MAX)
            return 0;
    }
    return 1;
}

static long parse_num(const char *s, long max)
{
    long v = 0;

    if (*s == '\0')
        return -1;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        v = v * 10 + (*s - '0');
        if (v > max)
            return -1;
    }
    return v;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* ---------------- 저널 ---------------- */

#define LOGQ_MAX 32
#define LOGQ_MSG 200

static long g_jh = -1;                   /* svc:/sys/journal 송신 핸들 */
static int g_jready;                     /* journald 가 "journal-ready" 를 보냈다 */
static unsigned long g_jpid;             /* 그 journald 의 pid */
static char g_logq[LOGQ_MAX][LOGQ_MSG];  /* 아직 보내지 못한 로그 메시지 */
static unsigned long g_logq_len[LOGQ_MAX];
static unsigned int g_logq_head;
static unsigned int g_logq_count;

static void journal_lost(void)
{
    if (g_jh >= 0)
        (void)nx_close(g_jh);
    g_jh = -1;
    g_jready = 0;
    g_jpid = 0;
}

/* 저널 채널로 보낸다 (attach >= 0 이면 핸들을 붙여). 큐가 가득 차면 NX_E_BUSY 그대로 */
static long journal_send(const void *msg, unsigned long len, long attach)
{
    long r;

    if (!g_jready)
        return NX_E_NOT_FOUND;
    if (g_jh < 0) {
        g_jh = nx_open(JOURNAL_SVC, NX_O_WRITE);
        if (g_jh < 0) {
            r = g_jh;
            g_jh = -1;
            return r;
        }
        set_cloexec(g_jh);
    }
    if (attach >= 0)
        r = nx_syscall(NX_SYS_CHANNEL_SEND_HANDLE, (unsigned long)g_jh, (unsigned long)msg, len,
                       (unsigned long)attach, 0);
    else
        r = nx_channel_send(g_jh, msg, len);
    if (r < 0 && r != NX_E_BUSY)
        journal_lost();                  /* journald 가 사라졌다: 다음 "journal-ready" 를 기다린다 */
    return r;
}

static void flush_logs(void)
{
    while (g_logq_count > 0 && g_jready) {
        unsigned int i = g_logq_head;

        /* 큐가 찼거나 journald 가 사라졌다: 이 메시지는 남겨 두고 다음에 */
        if (journal_send(g_logq[i], g_logq_len[i], -1) < 0)
            return;
        g_logq_head = (g_logq_head + 1U) % LOGQ_MAX;
        g_logq_count--;
    }
}

void ilog(int prio, const char *unit, const char *text)
{
    struct sbuf b;
    unsigned int slot;

    if (g_logq_count == LOGQ_MAX) {      /* 가득 찼다: 가장 오래된 것을 버린다 */
        g_logq_head = (g_logq_head + 1U) % LOGQ_MAX;
        g_logq_count--;
    }
    slot = (g_logq_head + g_logq_count) % LOGQ_MAX;
    sb_init(&b, g_logq[slot], LOGQ_MSG);
    sb_str(&b, "log");
    sb_nul(&b);
    sb_num(&b, prio);
    sb_nul(&b);
    sb_str(&b, "init");
    sb_nul(&b);
    sb_str(&b, unit != 0 ? unit : "");
    sb_nul(&b);
    sb_str(&b, text);
    g_logq_len[slot] = b.n;
    g_logq_count++;
    flush_logs();
}

void ilog3(int prio, const char *unit, const char *a, const char *b, const char *c)
{
    char text[LOGQ_MSG];
    struct sbuf s;

    sb_init(&s, text, sizeof(text));
    sb_str(&s, a);
    sb_str(&s, b);
    sb_str(&s, c);
    ilog(prio, unit, text);
}

/* 출력 파이프를 아직 넘기지 않은 유닛의 것을 journald 에 넘긴다 */
static void send_streams(void)
{
    int i;

    for (i = 0; i < UNIT_MAX && g_jready; i++) {
        struct unit *u = &g_units[i];
        char msg[96];
        struct sbuf b;

        if (!u->used || u->out_r < 0 || u->stream_sent)
            continue;
        sb_init(&b, msg, sizeof(msg));
        sb_str(&b, "stream");
        sb_nul(&b);
        sb_str(&b, u->name);
        sb_nul(&b);
        sb_num(&b, (long)u->pid);
        if (journal_send(msg, b.n, u->out_r) >= 0)
            u->stream_sent = 1;
        else
            return;                      /* 큐가 찼거나 journald 가 없다: 다음에 */
    }
}

static int streams_pending(void)
{
    int i;

    for (i = 0; i < UNIT_MAX; i++)
        if (g_units[i].used && g_units[i].out_r >= 0 && !g_units[i].stream_sent)
            return 1;
    return 0;
}

/* ---------------- nex 답장 ---------------- */

struct reply {
    long h;
    int dead;                            /* 받는 쪽이 읽지 않는다: 더 보내지 않는다 */
    char buf[NX_CHAN_MSG_MAX];
    unsigned long n;
};

/* 답장 채널로 보낸다. 큐가 차 있으면 조금(최대 0.5초) 기다린다: init 이 오래 묶이지 않게.
 * 보냈으면 0, 받는 쪽이 읽지 않거나 사라졌으면 -1 (그 답장의 나머지는 버린다) */
static int chan_send_wait(long h, const char *buf, unsigned long n)
{
    int tries;

    for (tries = 0; tries < 5; tries++) {
        struct nx_pollfd p;
        long r = nx_channel_send(h, buf, n);

        if (r != NX_E_BUSY)
            return r < 0 ? -1 : 0;
        p.handle = (unsigned long)h;
        p.events = NX_POLL_OUT;
        p.revents = 0;
        (void)nx_poll(&p, 1, 100);
    }
    return -1;
}

static void rp_begin(struct reply *r, long h)
{
    r->h = h;
    r->dead = 0;
    r->buf[0] = 'T';
    r->n = 1;
}

static void rp_flush(struct reply *r)
{
    if (r->n > 1 && r->h >= 0 && !r->dead && chan_send_wait(r->h, r->buf, r->n) < 0)
        r->dead = 1;
    r->n = 1;
}

static void rp_str(struct reply *r, const char *s)
{
    while (s != 0 && *s != '\0') {
        if (r->n == NX_CHAN_MSG_MAX)
            rp_flush(r);
        r->buf[r->n++] = *s++;
    }
}

static void rp_num(struct reply *r, long v)
{
    char tmp[24];
    struct sbuf b;

    sb_init(&b, tmp, sizeof(tmp));
    sb_num(&b, v);
    rp_str(r, tmp);
}

/* 마지막 결과를 보내고 핸들을 닫는다 */
static void rp_end(struct reply *r, int code)
{
    char e[8];

    rp_flush(r);
    e[0] = 'E';
    e[1] = (char)('0' + code);
    if (r->h >= 0) {
        if (!r->dead)
            (void)chan_send_wait(r->h, e, 2);
        (void)nx_close(r->h);
    }
    r->h = -1;
}

/* 글 한 줄과 결과로 바로 답한다 */
static void reply_now(long h, int code, const char *a, const char *b, const char *c)
{
    struct reply r;

    rp_begin(&r, h);
    rp_str(&r, a);
    rp_str(&r, b);
    rp_str(&r, c);
    if (a != 0 && a[0] != '\0')
        rp_str(&r, "\n");
    rp_end(&r, code);
}

/* 유닛의 기다리는 nex 에게 답한다 */
static void answer_waiter(struct unit *u, int code, const char *text)
{
    if (u->waiter >= 0) {
        if (code == NEX_OK)
            reply_now(u->waiter, code, "", "", "");
        else
            reply_now(u->waiter, code, text, u->name, " (see: nex status)");
        u->waiter = -1;
    }
}

/* ---------------- 유닛 파일 ---------------- */

static struct unit_cfg g_cfg;            /* 읽는 중인 유닛 (스택을 아끼려고 정적) */
static char g_filebuf[UNIT_FILE_MAX];

static void cfg_defaults(struct unit_cfg *c)
{
    nx_memset(c, 0, sizeof(*c));
    c->restart = RS_NO;
    c->restart_ms = 1000;
    c->stop_ms = 3000;
    c->output = OUT_JOURNAL;
    c->boot = 0;
    c->order = 50;
}

static int set_key(struct unit_cfg *c, const char *key, const char *val)
{
    long v;

    if (nx_strcmp(key, "description") == 0) {
        scopy(c->desc, sizeof(c->desc), val);
    } else if (nx_strcmp(key, "exec") == 0) {
        scopy(c->exec, sizeof(c->exec), val);
    } else if (nx_strcmp(key, "args") == 0) {
        scopy(c->args, sizeof(c->args), val);
    } else if (nx_strcmp(key, "restart") == 0) {
        if (nx_strcmp(val, "no") == 0)
            c->restart = RS_NO;
        else if (nx_strcmp(val, "on-failure") == 0)
            c->restart = RS_ON_FAILURE;
        else if (nx_strcmp(val, "always") == 0)
            c->restart = RS_ALWAYS;
        else
            return -1;
    } else if (nx_strcmp(key, "restart_ms") == 0) {
        if ((v = parse_num(val, 3600000)) < 0)
            return -1;
        c->restart_ms = (unsigned long)v;
    } else if (nx_strcmp(key, "stop_ms") == 0) {
        if ((v = parse_num(val, 600000)) < 0)
            return -1;
        c->stop_ms = (unsigned long)v;
    } else if (nx_strcmp(key, "output") == 0) {
        if (nx_strcmp(val, "journal") == 0)
            c->output = OUT_JOURNAL;
        else if (nx_strcmp(val, "console") == 0)
            c->output = OUT_CONSOLE;
        else
            return -1;
    } else if (nx_strcmp(key, "boot") == 0) {
        if (nx_strcmp(val, "yes") == 0)
            c->boot = 1;
        else if (nx_strcmp(val, "no") == 0)
            c->boot = 0;
        else
            return -1;
    } else if (nx_strcmp(key, "order") == 0) {
        if ((v = parse_num(val, 99)) < 0)
            return -1;
        c->order = (int)v;
    } else {
        return -1;
    }
    return 0;
}

/* 유닛 파일을 g_cfg 로 읽는다. 성공 0 (틀린 줄은 저널에 경고하고 건너뛴다) */
static int read_unit(const char *path, const char *name)
{
    long len = nx_read_value(path, g_filebuf, sizeof(g_filebuf));
    long i = 0;

    if (len < 0)
        return -1;
    cfg_defaults(&g_cfg);
    scopy(g_cfg.source, sizeof(g_cfg.source), path);
    while (i < len) {
        char *line = g_filebuf + i;
        char *key;
        char *val;
        char *eq;
        long end = i;

        while (end < len && g_filebuf[end] != '\n')
            end++;
        g_filebuf[end] = '\0';
        i = end + 1;

        while (is_space(*line))
            line++;
        if (*line == '\0' || *line == '#')
            continue;
        eq = line;
        while (*eq != '\0' && *eq != '=')
            eq++;
        if (*eq != '=') {
            ilog3(LOG_WARNING, name, "unit file ", path, ": line without '='");
            continue;
        }
        key = line;
        val = eq + 1;
        *eq = '\0';
        while (eq > key && is_space(eq[-1]))
            *--eq = '\0';
        while (is_space(*val))
            val++;
        {
            char *ve = val + nx_strlen(val);

            while (ve > val && is_space(ve[-1]))
                *--ve = '\0';
        }
        if (set_key(&g_cfg, key, val) < 0)
            ilog3(LOG_WARNING, name, "unit file: bad setting '", key, "'");
    }
    if (g_cfg.exec[0] == '\0') {
        ilog3(LOG_ERR, name, "unit file ", path, " has no exec: ignored");
        return -1;
    }
    return 0;
}

static struct unit *find_unit(const char *name)
{
    int i;

    for (i = 0; i < UNIT_MAX; i++)
        if (g_units[i].used && nx_strcmp(g_units[i].name, name) == 0)
            return &g_units[i];
    return 0;
}

static void unit_reset(struct unit *u)
{
    nx_memset(u, 0, sizeof(*u));
    u->proc = -1;
    u->out_r = -1;
    u->waiter = -1;
    u->state = ST_INACTIVE;
}

/* g_cfg 를 유닛 name 으로 등록한다 (있으면 설정만 바꾼다: 실행 중이면 다음 시작부터) */
static void install_unit(const char *name)
{
    struct unit *u = find_unit(name);
    int i;

    if (u == 0) {
        for (i = 0; i < UNIT_MAX; i++) {
            if (!g_units[i].used) {
                u = &g_units[i];
                unit_reset(u);
                u->used = 1;
                scopy(u->name, sizeof(u->name), name);
                u->since = now_ms();
                break;
            }
        }
        if (u == 0) {
            ilog3(LOG_ERR, name, "too many units: ", name, " ignored");
            return;
        }
    }
    nx_memcpy(&u->cfg, &g_cfg, sizeof(g_cfg));
    u->found = 1;
}

static void scan_dir(const char *dir)
{
    unsigned long i;
    struct nx_dirent de;
    char path[NX_PATH_MAX];

    for (i = 0; nx_read_dir(dir, i, &de) >= 0; i++) {
        struct sbuf b;

        if (!valid_name(de.name))
            continue;
        sb_init(&b, path, sizeof(path));
        sb_str(&b, dir);
        sb_str(&b, "/");
        sb_str(&b, de.name);
        if (read_unit(path, de.name) == 0)
            install_unit(de.name);
    }
}

void svc_load(void)
{
    int i;

    for (i = 0; i < UNIT_MAX; i++)
        g_units[i].found = 0;
    scan_dir(UNITS_DEFAULT);
    scan_dir(UNITS_ADMIN);
    /* 파일이 사라진 유닛: 멈춰 있으면 지우고, 돌고 있으면 멈출 때까지 둔다 */
    for (i = 0; i < UNIT_MAX; i++) {
        struct unit *u = &g_units[i];

        if (u->used && !u->found) {
            if (u->proc < 0 && u->waiter < 0)
                u->used = 0;
            else
                scopy(u->cfg.source, sizeof(u->cfg.source), "(deleted)");
        }
    }
}

/* 부팅 때 시작할지: roi:/app/0/init/enabled/<이름> 이 있으면 그것, 없으면 유닛의 boot */
static int unit_enabled(const struct unit *u)
{
    char path[NX_PATH_MAX];
    char val[16];
    struct sbuf b;

    sb_init(&b, path, sizeof(path));
    sb_str(&b, ENABLED_DIR "/");
    sb_str(&b, u->name);
    if (nx_read_value(path, val, sizeof(val)) >= 0) {
        if (nx_strcmp(val, "yes") == 0)
            return 1;
        if (nx_strcmp(val, "no") == 0)
            return 0;
    }
    return u->cfg.boot;
}

static long set_enabled(const struct unit *u, int on)
{
    char path[NX_PATH_MAX];
    struct sbuf b;
    long h;
    long r;

    sb_init(&b, path, sizeof(path));
    sb_str(&b, ENABLED_DIR "/");
    sb_str(&b, u->name);
    r = nx_create_node(ENABLED_DIR, NX_NODE_DIR, 0700);
    if (r < 0 && r != NX_E_EXISTS)
        return r;
    r = nx_create_node(path, NX_NODE_FILE, 0600);
    if (r < 0 && r != NX_E_EXISTS)
        return r;
    h = nx_open(path, NX_O_WRITE | NX_O_TRUNC);
    if (h < 0)
        return h;
    r = nx_write(h, on ? "yes\n" : "no\n", on ? 4 : 3);
    (void)nx_close(h);
    return r < 0 ? r : 0;
}

/* ---------------- 시작과 멈춤 ---------------- */

static char g_argbuf[256];

/* fork 한 자식에서: 0/1/2 를 맞추고 exec. 돌아오지 않는다 */
static void child_exec(struct unit *u, long in_r, long out_w) __attribute__((noreturn));
static void child_exec(struct unit *u, long in_r, long out_w)
{
    const char *argv[UNIT_ARGV_MAX + 1];
    const char *envp[6];
    char prog[UNIT_NAME_MAX];
    char env_unit[UNIT_NAME_MAX + 16];
    struct sbuf b;
    int argc = 0;
    char *p;
    const char *base;
    long err = 2;
    unsigned long k;

    /* 새 프로세스 그룹: 콘솔의 Ctrl-C(포그라운드 그룹 신호)가 서비스에 닿지 않게 */
    (void)nx_syscall(NX_SYS_PROCESS_GROUP, NX_PID_SELF, 0, 0, 0, 0);
    (void)nx_syscall(NX_SYS_DUPLICATE_HANDLE_EX, (unsigned long)in_r, NX_RIGHT_ALL,
                     NX_DUP_SHARE | NX_DUP_TO_SLOT, 0, 0);
    if (out_w >= 0) {
        (void)nx_syscall(NX_SYS_DUPLICATE_HANDLE_EX, (unsigned long)out_w, NX_RIGHT_ALL,
                         NX_DUP_SHARE | NX_DUP_TO_SLOT, 1, 0);
        err = nx_syscall(NX_SYS_DUPLICATE_HANDLE_EX, (unsigned long)out_w, NX_RIGHT_ALL,
                         NX_DUP_SHARE | NX_DUP_TO_SLOT, 2, 0);
    }

    /* argv[0] = 실행 파일 이름에서 ".run" 을 뗀 것 */
    base = u->cfg.exec;
    for (p = u->cfg.exec; *p != '\0'; p++)
        if (*p == '/')
            base = p + 1;
    scopy(prog, sizeof(prog), base);
    k = nx_strlen(prog);
    if (k > 4 && nx_strcmp(prog + k - 4, ".run") == 0)
        prog[k - 4] = '\0';
    argv[argc++] = prog;

    /* args: 공백으로 나누고 "..." 는 하나로 */
    scopy(g_argbuf, sizeof(g_argbuf), u->cfg.args);
    p = g_argbuf;
    while (*p != '\0' && argc < UNIT_ARGV_MAX) {
        char *start;

        while (is_space(*p))
            p++;
        if (*p == '\0')
            break;
        if (*p == '"') {
            start = ++p;
            while (*p != '\0' && *p != '"')
                p++;
        } else {
            start = p;
            while (*p != '\0' && !is_space(*p))
                p++;
        }
        if (*p != '\0')
            *p++ = '\0';
        argv[argc++] = start;
    }
    argv[argc] = 0;

    sb_init(&b, env_unit, sizeof(env_unit));
    sb_str(&b, "NEX_UNIT=");
    sb_str(&b, u->name);
    envp[0] = "USER=root";
    envp[1] = "LOGNAME=root";
    envp[2] = "SHELL=app:/nysh.run";
    envp[3] = env_unit;
    envp[4] = 0;

    (void)nx_syscall(NX_SYS_PROCESS_EXEC, (unsigned long)u->cfg.exec, (unsigned long)argv,
                     (unsigned long)envp, 0, 0);
    if (err >= 0) {
        static const char msg[] = "init: exec failed\n";

        (void)nx_write(err, msg, sizeof(msg) - 1);
    }
    nx_exit(127);
}

static void set_state(struct unit *u, int st)
{
    u->state = st;
    u->since = now_ms();
}

/* 유닛을 시작한다. 성공 0, 실패면 음수 (유닛은 failed) */
static long start_unit(struct unit *u)
{
    unsigned long pipe_in[2];
    unsigned long pipe_out[2];
    struct nx_stat st;
    struct nx_procinfo pi;
    long out_r = -1;
    long out_w = -1;
    long proc;
    long r;

    u->then_start = 0;
    u->killed = 0;
    r = nx_syscall(NX_SYS_STAT_PATH, (unsigned long)u->cfg.exec, (unsigned long)&st, 0, 0, 0);
    if (r < 0) {
        scopy(u->result, sizeof(u->result), "exec-not-found");
        set_state(u, ST_FAILED);
        ilog3(LOG_ERR, u->name, "Failed to start ", u->name, ": exec not found");
        return r;
    }

    r = nx_syscall(NX_SYS_PIPE_CREATE, (unsigned long)pipe_in, NX_DUP_CLOEXEC, 0, 0, 0);
    if (r < 0)
        goto fail;
    if (u->cfg.output == OUT_JOURNAL) {
        r = nx_syscall(NX_SYS_PIPE_CREATE, (unsigned long)pipe_out, NX_DUP_CLOEXEC, 0, 0, 0);
        if (r < 0) {
            (void)nx_close((long)pipe_in[0]);
            (void)nx_close((long)pipe_in[1]);
            goto fail;
        }
        out_r = (long)pipe_out[0];
        out_w = (long)pipe_out[1];
    }

    proc = nx_syscall(NX_SYS_PROCESS_FORK, 0, 0, 0, 0, 0);
    if (proc == 0)
        child_exec(u, (long)pipe_in[0], out_w);

    (void)nx_close((long)pipe_in[0]);
    (void)nx_close((long)pipe_in[1]);
    if (out_w >= 0)
        (void)nx_close(out_w);
    if (proc < 0) {
        if (out_r >= 0)
            (void)nx_close(out_r);
        r = proc;
        goto fail;
    }
    set_cloexec(proc);
    u->proc = proc;
    u->pid = nx_process_info(proc, &pi) == 0 ? pi.pid : 0;
    u->out_r = out_r;
    u->stream_sent = 0;
    set_state(u, ST_ACTIVE);
    {
        char pid[24];
        struct sbuf b;

        sb_init(&b, pid, sizeof(pid));
        sb_str(&b, " (pid ");
        sb_num(&b, (long)u->pid);
        sb_str(&b, ").");
        ilog3(LOG_INFO, u->name, "Started ", u->name, pid);
    }
    send_streams();
    return 0;

fail:
    scopy(u->result, sizeof(u->result), "resources");
    set_state(u, ST_FAILED);
    ilog3(LOG_ERR, u->name, "Failed to start ", u->name, ": out of resources");
    return r < 0 ? r : NX_E_BUSY;
}

static void begin_stop(struct unit *u)
{
    u->killed = 0;
    set_state(u, ST_STOPPING);
    u->deadline = u->since + u->cfg.stop_ms;
    if (nx_syscall(NX_SYS_SIGNAL_SEND, (unsigned long)u->proc, NX_SIGTERM, 0, 0, 0) < 0) {
        (void)nx_kill(u->proc);
        u->killed = 1;
    }
    ilog3(LOG_INFO, u->name, "Stopping ", u->name, "...");
}

/* 자동으로 다시 시작해도 되는지 (10초 안에 5번까지) */
static int burst_ok(struct unit *u)
{
    unsigned long t = now_ms();

    if (u->burst == 0 || t - u->burst_start > BURST_WINDOW_MS) {
        u->burst_start = t;
        u->burst = 0;
    }
    u->burst++;
    return u->burst <= BURST_MAX;
}

static void exit_text(struct sbuf *b, const struct unit *u)
{
    if (u->code == NX_EXIT_KILLED) {
        sb_str(b, "killed");
    } else if (u->code == NX_EXIT_FAULT) {
        sb_str(b, "crashed (CPU fault)");
    } else if (NX_EXIT_IS_SIGNAL(u->code)) {
        sb_str(b, "signal ");
        sb_num(b, -256 - u->code);
    } else {
        sb_str(b, "code ");
        sb_num(b, u->code);
    }
}

void svc_exited(int i)
{
    struct unit *u = &g_units[i];
    int code = 0;
    int was_stopping = u->state == ST_STOPPING;
    char text[96];
    struct sbuf b;

    if (u->proc < 0)
        return;
    if (nx_wait(u->proc, 0, &code) < 0)
        code = NX_EXIT_KILLED;
    (void)nx_close(u->proc);
    u->proc = -1;
    if (g_jready && g_jpid != 0 && u->pid == g_jpid)
        journal_lost();                  /* 그 journald 가 끝났다 */
    u->pid = 0;
    if (u->out_r >= 0)
        (void)nx_close(u->out_r);       /* journald 의 사본이 남은 것을 마저 읽는다 */
    u->out_r = -1;
    u->stream_sent = 0;
    u->has_code = 1;
    u->code = code;

    if (code == 0)
        scopy(u->result, sizeof(u->result), "success");
    else if (code == NX_EXIT_KILLED || NX_EXIT_IS_SIGNAL(code))
        scopy(u->result, sizeof(u->result), "signal");
    else if (code == NX_EXIT_FAULT)
        scopy(u->result, sizeof(u->result), "core-dump");
    else
        scopy(u->result, sizeof(u->result), "exit-code");

    sb_init(&b, text, sizeof(text));
    if (was_stopping) {
        ilog3(LOG_INFO, u->name, "Stopped ", u->name, ".");
        if (u->then_start) {
            u->burst = 0;
            if (start_unit(u) == 0)
                answer_waiter(u, NEX_OK, "restarted ");
            else
                answer_waiter(u, NEX_FAIL, "failed to restart ");
        } else {
            set_state(u, ST_INACTIVE);
            answer_waiter(u, NEX_OK, "stopped ");
        }
        return;
    }

    sb_str(&b, u->name);
    sb_str(&b, ": main process exited, ");
    exit_text(&b, u);
    ilog(code == 0 ? LOG_INFO : LOG_WARNING, u->name, text);

    if (u->cfg.restart == RS_ALWAYS || (u->cfg.restart == RS_ON_FAILURE && code != 0)) {
        if (burst_ok(u)) {
            set_state(u, ST_RESTARTING);
            u->deadline = u->since + u->cfg.restart_ms;
            return;
        }
        scopy(u->result, sizeof(u->result), "start-limit-hit");
        set_state(u, ST_FAILED);
        ilog3(LOG_ERR, u->name, "Start request repeated too quickly: ", u->name, " failed.");
        nx_puts("init: unit ");
        nx_puts(u->name);
        nx_puts(" keeps failing (nex status ");
        nx_puts(u->name);
        nx_puts(")\n");
        return;
    }
    set_state(u, code == 0 ? ST_INACTIVE : ST_FAILED);
}

unsigned long svc_poll_fill(struct nx_pollfd *fds, int *unit_of, unsigned long n, unsigned long max)
{
    unsigned long added = 0;
    int i;

    for (i = 0; i < UNIT_MAX && n + added < max; i++) {
        if (!g_units[i].used || g_units[i].proc < 0)
            continue;
        fds[n + added].handle = (unsigned long)g_units[i].proc;
        fds[n + added].events = NX_POLL_EXIT;
        fds[n + added].revents = 0;
        unit_of[n + added] = i;
        added++;
    }
    return added;
}

unsigned long svc_timeout_ms(void)
{
    unsigned long t = now_ms();
    unsigned long best = NX_WAIT_INFINITE;
    int i;

    if (g_jready && (g_logq_count > 0 || streams_pending()))
        best = 100;                      /* journald 의 큐가 찼었다: 곧 다시 */
    for (i = 0; i < UNIT_MAX; i++) {
        struct unit *u = &g_units[i];
        unsigned long left;

        if (!u->used)
            continue;
        if (u->state == ST_RESTARTING || (u->state == ST_STOPPING && !u->killed)) {
            left = u->deadline > t ? u->deadline - t : 0;
            if (left < best)
                best = left;
        }
    }
    return best;
}

void svc_tick(void)
{
    unsigned long t = now_ms();
    int i;

    for (i = 0; i < UNIT_MAX; i++) {
        struct unit *u = &g_units[i];

        if (!u->used)
            continue;
        if (u->state == ST_STOPPING && !u->killed && t >= u->deadline) {
            ilog3(LOG_WARNING, u->name, "Stop timed out: killing ", u->name, ".");
            (void)nx_kill(u->proc);
            u->killed = 1;
        } else if (u->state == ST_RESTARTING && t >= u->deadline) {
            u->restarts++;
            (void)start_unit(u);
        }
    }
    if (g_jready) {
        flush_logs();
        send_streams();
    }
}

/* boot 가 켜진 유닛을 order, 이름 순서로 시작한다 */
void svc_boot(void)
{
    int done[UNIT_MAX];
    int i;

    for (i = 0; i < UNIT_MAX; i++)
        done[i] = 0;
    for (;;) {
        int best = -1;

        for (i = 0; i < UNIT_MAX; i++) {
            struct unit *u = &g_units[i];

            if (!u->used || done[i])
                continue;
            if (best < 0 || u->cfg.order < g_units[best].cfg.order ||
                (u->cfg.order == g_units[best].cfg.order && nx_strcmp(u->name, g_units[best].name) < 0))
                best = i;
        }
        if (best < 0)
            break;
        done[best] = 1;
        if (unit_enabled(&g_units[best]))
            (void)start_unit(&g_units[best]);
    }
}

/* ---------------- nex 요청 ---------------- */

static void nex_list(long h)
{
    struct reply r;
    int done[UNIT_MAX];
    int i;

    for (i = 0; i < UNIT_MAX; i++)
        done[i] = 0;
    rp_begin(&r, h);
    for (;;) {
        int best = -1;
        struct unit *u;

        for (i = 0; i < UNIT_MAX; i++)
            if (g_units[i].used && !done[i] && (best < 0 || nx_strcmp(g_units[i].name, g_units[best].name) < 0))
                best = i;
        if (best < 0)
            break;
        done[best] = 1;
        u = &g_units[best];
        rp_str(&r, u->name);
        rp_str(&r, "\t");
        rp_str(&r, g_state_names[u->state]);
        rp_str(&r, "\t");
        rp_num(&r, (long)u->pid);
        rp_str(&r, "\t");
        rp_str(&r, unit_enabled(u) ? "enabled" : "disabled");
        rp_str(&r, "\t");
        rp_str(&r, u->cfg.desc);
        rp_str(&r, "\n");
    }
    rp_end(&r, NEX_OK);
}

static void kv(struct reply *r, const char *key, const char *val)
{
    rp_str(r, key);
    rp_str(r, "\t");
    rp_str(r, val);
    rp_str(r, "\n");
}

static void kvn(struct reply *r, const char *key, long v)
{
    rp_str(r, key);
    rp_str(r, "\t");
    rp_num(r, v);
    rp_str(r, "\n");
}

static void nex_status(long h, struct unit *u)
{
    struct reply r;

    rp_begin(&r, h);
    kv(&r, "name", u->name);
    kv(&r, "description", u->cfg.desc);
    kv(&r, "source", u->cfg.source);
    kv(&r, "enabled", unit_enabled(u) ? "enabled" : "disabled");
    kv(&r, "boot", u->cfg.boot ? "yes" : "no");
    kv(&r, "state", g_state_names[u->state]);
    kvn(&r, "since_ms", (long)(now_ms() - u->since));
    if (u->pid != 0)
        kvn(&r, "pid", (long)u->pid);
    kv(&r, "exec", u->cfg.exec);
    kv(&r, "args", u->cfg.args);
    kv(&r, "restart", g_restart_names[u->cfg.restart]);
    kvn(&r, "restarts", (long)u->restarts);
    kv(&r, "output", g_output_names[u->cfg.output]);
    if (u->has_code) {
        char text[48];
        struct sbuf b;

        sb_init(&b, text, sizeof(text));
        exit_text(&b, u);
        kv(&r, "last_exit", text);
    }
    if (u->result[0] != '\0')
        kv(&r, "result", u->result);
    rp_end(&r, NEX_OK);
}

static void nex_start(long h, struct unit *u)
{
    if (u->waiter >= 0) {
        reply_now(h, NEX_BUSY, "another nex command is in progress for ", u->name, "");
        return;
    }
    switch (u->state) {
    case ST_ACTIVE:
        reply_now(h, NEX_OK, "", "", "");
        return;
    case ST_STOPPING:
        u->then_start = 1;
        u->waiter = h;
        return;
    default:
        u->burst = 0;
        if (start_unit(u) == 0)
            reply_now(h, NEX_OK, "", "", "");
        else
            reply_now(h, NEX_FAIL, "failed to start ", u->name, " (see: nex status)");
        return;
    }
}

static void nex_stop(long h, struct unit *u, int then_start)
{
    if (u->waiter >= 0) {
        reply_now(h, NEX_BUSY, "another nex command is in progress for ", u->name, "");
        return;
    }
    switch (u->state) {
    case ST_ACTIVE:
        begin_stop(u);
        u->then_start = then_start;
        u->waiter = h;
        return;
    case ST_STOPPING:
        u->then_start = then_start;
        u->waiter = h;
        return;
    default:
        if (then_start) {
            u->burst = 0;
            if (start_unit(u) == 0)
                reply_now(h, NEX_OK, "", "", "");
            else
                reply_now(h, NEX_FAIL, "failed to start ", u->name, " (see: nex status)");
        } else {
            if (u->state != ST_INACTIVE)
                set_state(u, ST_INACTIVE);     /* failed / restarting 을 끝낸다 */
            reply_now(h, NEX_OK, "", "", "");
        }
        return;
    }
}

/* "nex\0명령\0유닛\0" */
static void nex_request(const char *msg, unsigned long len, const struct nx_msginfo *info, long h)
{
    const char *f[4];
    unsigned long nf = 0;
    unsigned long start = 4;
    unsigned long i;
    const char *cmd;
    const char *arg;
    struct unit *u;
    int admin;

    if (msg[len - 1] != '\0') {
        reply_now(h, NEX_USAGE, "bad request", "", "");
        return;
    }
    for (i = start; i < len && nf < 4; i++) {
        if (msg[i] == '\0') {
            f[nf++] = msg + start;
            start = i + 1;
        }
    }
    if (nf < 2) {
        reply_now(h, NEX_USAGE, "bad request", "", "");
        return;
    }
    cmd = f[0];
    arg = f[1];
    admin = nx_strcmp(cmd, "start") == 0 || nx_strcmp(cmd, "stop") == 0 || nx_strcmp(cmd, "restart") == 0 ||
            nx_strcmp(cmd, "enable") == 0 || nx_strcmp(cmd, "disable") == 0 || nx_strcmp(cmd, "reload") == 0;
    if (admin && info->sender_uid != 0) {
        reply_now(h, NEX_PERM, "permission denied (try: elev nex ", cmd, " ...)");
        return;
    }

    if (nx_strcmp(cmd, "list") == 0) {
        nex_list(h);
        return;
    }
    if (nx_strcmp(cmd, "reload") == 0) {
        svc_load();
        ilog(LOG_INFO, 0, "Reloaded unit files.");
        reply_now(h, NEX_OK, "", "", "");
        return;
    }

    u = find_unit(arg);
    if (u == 0) {
        reply_now(h, NEX_NOT_FOUND, "unit ", arg, " not found");
        return;
    }
    if (nx_strcmp(cmd, "status") == 0) {
        nex_status(h, u);
    } else if (nx_strcmp(cmd, "start") == 0) {
        nex_start(h, u);
    } else if (nx_strcmp(cmd, "stop") == 0) {
        nex_stop(h, u, 0);
    } else if (nx_strcmp(cmd, "restart") == 0) {
        nex_stop(h, u, 1);
    } else if (nx_strcmp(cmd, "enable") == 0 || nx_strcmp(cmd, "disable") == 0) {
        int on = cmd[0] == 'e';

        if (set_enabled(u, on) < 0) {
            reply_now(h, NEX_FAIL, "cannot write " ENABLED_DIR "/", u->name, "");
        } else {
            ilog3(LOG_INFO, u->name, on ? "Enabled " : "Disabled ", u->name, ".");
            reply_now(h, NEX_OK, "", "", "");
        }
    } else {
        reply_now(h, NEX_USAGE, "unknown command: ", cmd, "");
    }
}

int svc_request(const char *msg, unsigned long len, const struct nx_msginfo *info, long *attached)
{
    if (len == 13 && nx_memcmp(msg, "journal-ready", 13) == 0) {
        if (info->sender_uid == 0) {
            int i;

            if (g_jh >= 0)
                (void)nx_close(g_jh);   /* 옛 journald 의 채널일 수 있다 */
            g_jh = -1;
            g_jready = 1;
            g_jpid = info->sender_pid;
            for (i = 0; i < UNIT_MAX; i++)
                g_units[i].stream_sent = 0;   /* 새 journald 에는 아무것도 넘기지 않았다 */
            send_streams();
            flush_logs();
        }
        return 1;
    }
    if (len >= 4 && nx_memcmp(msg, "nex", 4) == 0) {
        long h = *attached;
        struct nx_stat st;

        /* 답장은 붙어 온 채널 송신 핸들로만 */
        if (h < 0 || nx_stat(h, &st) < 0 || st.type != NX_TYPE_CHANNEL || !(st.rights & NX_RIGHT_SEND))
            return 1;
        set_cloexec(h);
        *attached = -1;
        nex_request(msg, len, info, h);
        return 1;
    }
    return 0;
}
