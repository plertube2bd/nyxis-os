/*
 * ntop - 프로세스 관리 (nx64api 의 TUI + 프로세스 목록)
 *
 *   ntop            전체 화면. 1초마다 새로 읽는다
 *   ntop -b         지금 목록을 한 번 표로 찍고 끝 (스크립트용: CPU% 대신 누적 CPU 시간)
 *
 * 보이는 것: 사용자 프로세스마다 PID, 부모, 사용자, 상태(R 실행 / S 대기 / W 준비), CPU%(지난 갱신 이후), 메모리,
 * 누적 CPU 시간, 이름. 이름 뒤의 표시: "[system]" 보호 프로세스(init, login: 끝낼 수 없다), "[login]" 콘솔 세션(로그인 셸),
 * "[pty]" 의사 터미널에서 도는 것. 커널 스레드(idle: 할 일이 없을 때 CPU 가 도는 자리)는 목록에 넣지 않고 맨 위의
 * "idle" 로만 보인다. 맨 위: CPU 사용률(idle 이 아닌 몫), idle 몫, 메모리, 프로세스 수, 업타임.
 *
 * 키
 *   ↑ ↓ PgUp PgDn Home End  고르기
 *   k   SIGTERM (끝내 달라고 부탁)         K   SIGKILL (y 로 확인)       i  SIGINT
 *   s   정렬 바꾸기: PID -> CPU -> 메모리   q / Esc  끝
 * 신호는 자기 프로세스에만 보낼 수 있다 (root 는 모두): 커널의 NxSignalSendPid 가 판단한다.
 *
 * 설정 (nx64_setting_get: 사용자 값 roi:/<uid>/app/ntop/<키>, 기본값 app:/ntop.run/<키>):
 *   refresh_ms   다시 읽는 간격 (기본 1000, 200~10000)
 *   sort         처음 정렬: pid | cpu | mem (기본 pid)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <nx64.h>

#define MAX_PROCS 64

enum sort { SORT_PID, SORT_CPU, SORT_MEM };

struct row {
    struct nx64_proc p;
    unsigned long cpuDelta;             /* 지난 갱신 이후의 CPU 틱 */
};

struct state {
    struct row rows[MAX_PROCS];
    int count;
    int total;                          /* 커널이 알려 준 전체 수 (MAX_PROCS 보다 클 수 있다) */
    unsigned long prevPid[MAX_PROCS];
    unsigned long prevTicks[MAX_PROCS];
    int prevCount;
    unsigned long sumDelta;             /* 모든 프로세스(idle 포함)의 CPU 틱 합: CPU% 의 분모 */
    unsigned long idleDelta;
    unsigned long hz;
    enum sort sort;
    unsigned long selPid;               /* 고른 프로세스 (pid 로 기억: 정렬/갱신에도 따라간다) */
    int selPos;                         /* 그 줄의 자리: 그 프로세스가 끝나 사라지면 같은 자리를 고른다 */
    int top;                            /* 목록의 맨 위에 보이는 줄 */
    int listRows;
    unsigned long confirmPid;           /* SIGKILL 확인 중 (0 = 아님) */
    char msg[96];
    struct nx64_identity me;
};

/* ---------------- 목록 ---------------- */

static unsigned long prevTicksOf(const struct state *st, unsigned long pid, int *found)
{
    int i;

    for (i = 0; i < st->prevCount; i++) {
        if (st->prevPid[i] == pid) {
            *found = 1;
            return st->prevTicks[i];
        }
    }
    *found = 0;
    return 0;
}

static int selIndex(const struct state *st);

/* 고른 프로세스가 사라졌으면 같은 자리의 줄을 고른다 */
static void fixSelection(struct state *st)
{
    int i = selIndex(st);

    if (i >= 0) {
        st->selPid = st->rows[i].p.pid;
        st->selPos = i;
    }
}

static int cmpRows(const void *a, const void *b, enum sort s)
{
    const struct row *x = (const struct row *)a;
    const struct row *y = (const struct row *)b;

    if (s == SORT_CPU && x->cpuDelta != y->cpuDelta)
        return x->cpuDelta > y->cpuDelta ? -1 : 1;
    if (s == SORT_MEM && x->p.mem_kib != y->p.mem_kib)
        return x->p.mem_kib > y->p.mem_kib ? -1 : 1;
    return x->p.pid < y->p.pid ? -1 : x->p.pid > y->p.pid ? 1 : 0;
}

static enum sort g_sortKey;

static int cmpQsort(const void *a, const void *b)
{
    return cmpRows(a, b, g_sortKey);
}

/* 다시 읽는다. CPU% 는 지난 읽기와의 차이로 */
static void refresh(struct state *st)
{
    struct nx64_proc procs[MAX_PROCS];
    int n = nx64_processes(procs, MAX_PROCS);
    int i;

    if (n < 0) {
        snprintf(st->msg, sizeof(st->msg), "cannot read processes: %s", strerror(errno));
        return;
    }
    if (n > MAX_PROCS)
        n = MAX_PROCS;
    st->sumDelta = 0;
    st->idleDelta = 0;
    for (i = 0; i < n; i++) {
        int found;
        unsigned long before = prevTicksOf(st, procs[i].pid, &found);

        st->rows[i].p = procs[i];
        /* 처음 보는 프로세스는 시작한 뒤의 전부가 이번 몫이다 (이전 기록이 없으면 0 으로 시작해 첫 화면만 0%) */
        st->rows[i].cpuDelta = found && procs[i].cpu_ticks >= before ? procs[i].cpu_ticks - before
                               : (st->prevCount > 0 ? procs[i].cpu_ticks : 0);
        st->sumDelta += st->rows[i].cpuDelta;
        if (!(procs[i].flags & NX64_PROC_USER))
            st->idleDelta += st->rows[i].cpuDelta;      /* 커널 스레드 (idle) */
    }
    /* 목록에는 사용자 프로세스만 */
    st->count = 0;
    st->total = 0;
    for (i = 0; i < n; i++) {
        if (st->rows[i].p.flags & NX64_PROC_USER) {
            st->rows[st->count++] = st->rows[i];
            st->total++;
        }
    }
    for (i = 0; i < n; i++) {
        st->prevPid[i] = procs[i].pid;
        st->prevTicks[i] = procs[i].cpu_ticks;
    }
    st->prevCount = n;
    g_sortKey = st->sort;
    qsort(st->rows, (size_t)st->count, sizeof(st->rows[0]), cmpQsort);
    fixSelection(st);
}

static int selIndex(const struct state *st)
{
    int i;

    for (i = 0; i < st->count; i++) {
        if (st->rows[i].p.pid == st->selPid)
            return i;
    }
    if (st->count == 0)
        return -1;
    return st->selPos < st->count ? st->selPos : st->count - 1;
}

/* ---------------- 서식 ---------------- */

static char stateChar(int s)
{
    switch (s) {
    case NX64_PROC_RUNNING: return 'R';
    case NX64_PROC_BLOCKED: return 'S';
    default:                return 'W';
    }
}

static void fmtTime(unsigned long ticks, unsigned long hz, char *out, size_t cap)
{
    unsigned long cs = hz ? ticks * 100UL / hz : 0;       /* 1/100 초 */
    unsigned long s = cs / 100UL;

    if (s >= 3600UL)
        snprintf(out, cap, "%luh%02lum", s / 3600UL, s / 60UL % 60UL);
    else
        snprintf(out, cap, "%lu:%02lu.%02lu", s / 60UL, s % 60UL, cs % 100UL);
}

static void fmtMem(unsigned long kib, char *out, size_t cap)
{
    if (kib >= 10UL * 1024UL)
        snprintf(out, cap, "%luM", kib / 1024UL);
    else
        snprintf(out, cap, "%luK", kib);
}

static void fmtName(const struct nx64_proc *p, char *out, size_t cap)
{
    snprintf(out, cap, "%s%s%s%s", p->name, (p->flags & NX64_PROC_CRITICAL) ? " [system]" : "",
             (p->flags & NX64_PROC_SESSION) ? " [login]" : "", (p->flags & NX64_PROC_PTY) ? " [pty]" : "");
}

/* ---------------- 그리기 ---------------- */

static void draw(void *ctx)
{
    struct state *st = (struct state *)ctx;
    struct nx64_memory m;
    int cols = nx64_tui_cols();
    int rows = nx64_tui_rows();
    unsigned long up = nx64_uptime_ms() / 1000UL;
    unsigned long busy = st->sumDelta > st->idleDelta ? st->sumDelta - st->idleDelta : 0;
    int sel = selIndex(st);
    int y;
    int i;
    char a[24];
    char b[24];
    char name[64];
    static const char *const sortName[] = { "pid", "cpu", "mem" };

    nx64_tui_clear(NX64_DEFAULT);
    nx64_tui_textf(0, 0, NX64_ACCENT, " ntop ");
    nx64_tui_textf(7, 0, NX64_DEFAULT, "%s  up %lud %02lu:%02lu:%02lu  %d processes  idle %lu%%", st->me.name,
                   up / 86400UL, up / 3600UL % 24UL, up / 60UL % 60UL, up % 60UL, st->total,
                   st->sumDelta ? st->idleDelta * 100UL / st->sumDelta : 100UL);

    /* CPU / 메모리 막대 */
    {
        int bw = cols / 2 - 14;
        unsigned long pct = st->sumDelta ? busy * 100UL / st->sumDelta : 0;

        if (bw < 4)
            bw = 4;
        nx64_tui_textf(1, 1, NX64_DEFAULT, "CPU %3lu%%", pct);
        nx64_tui_bar(10, 1, bw, busy, st->sumDelta ? st->sumDelta : 1, nx64_style_make(231, 54, 0),
                     nx64_style_make(250, 236, 0));
        if (nx64_memory(&m) == 0 && m.ram_kib > 0) {
            int mx = 10 + bw + 2;

            fmtMem(m.used_kib, a, sizeof(a));
            fmtMem(m.ram_kib, b, sizeof(b));
            nx64_tui_textf(mx, 1, NX64_DEFAULT, "Mem %s/%s", a, b);
            nx64_tui_bar(mx + 18, 1, cols - mx - 19, m.used_kib, m.ram_kib, nx64_style_make(231, 23, 0),
                         nx64_style_make(250, 236, 0));
        }
    }

    /* 목록 */
    nx64_tui_fill(0, 3, cols, 1, ' ', NX64_FRAME);
    nx64_tui_textf(0, 3, NX64_FRAME, "%6s %6s %-10s S %5s %7s %9s  NAME   (sort: %s)", "PID", "PPID", "USER",
                   "CPU%", "MEM", "TIME", sortName[st->sort]);
    st->listRows = rows - 6;
    if (st->listRows < 1)
        st->listRows = 1;
    if (sel >= 0) {
        if (sel < st->top)
            st->top = sel;
        if (sel >= st->top + st->listRows)
            st->top = sel - st->listRows + 1;
    }
    if (st->top > st->count - st->listRows)
        st->top = st->count - st->listRows > 0 ? st->count - st->listRows : 0;

    for (y = 0, i = st->top; y < st->listRows && i < st->count; y++, i++) {
        const struct row *r = &st->rows[i];
        unsigned long pct10 = st->sumDelta ? r->cpuDelta * 1000UL / st->sumDelta : 0;
        nx64_style s = i == sel ? NX64_SELECTED : (r->p.flags & NX64_PROC_CRITICAL) ? NX64_DIM : NX64_DEFAULT;

        fmtMem(r->p.mem_kib, a, sizeof(a));
        fmtTime(r->p.cpu_ticks, st->hz, b, sizeof(b));
        fmtName(&r->p, name, sizeof(name));
        if (i == sel)
            nx64_tui_fill(0, 4 + y, cols, 1, ' ', s);
        nx64_tui_textf(0, 4 + y, s, "%6lu %6lu %-10.10s %c %3lu.%lu %7s %9s  %s", r->p.pid, r->p.ppid,
                       r->p.user[0] ? r->p.user : "-", stateChar(r->p.state), pct10 / 10UL, pct10 % 10UL,
                       a, b, name);
    }

    /* 아래: 안내 / 확인 / 결과 */
    if (st->confirmPid != 0) {
        nx64_tui_textf(1, rows - 1, nx64_style_make(231, 88, NX64_BOLD), " kill %lu with SIGKILL? y = yes ",
                       st->confirmPid);
    } else if (st->msg[0]) {
        nx64_tui_text(1, rows - 1, st->msg, NX64_ACCENT);
    } else {
        nx64_tui_text(1, rows - 1, "Up/Down select  k term  K kill  i interrupt  s sort  q quit", NX64_DIM);
    }
}

/* ---------------- 키 ---------------- */

static void sendSignal(struct state *st, unsigned long pid, int sig, const char *what)
{
    int i;
    int critical = 0;

    for (i = 0; i < st->count; i++) {
        if (st->rows[i].p.pid == pid)
            critical = (st->rows[i].p.flags & NX64_PROC_CRITICAL) != 0;
    }
    if (nx64_signal_pid(pid, sig) == 0)
        snprintf(st->msg, sizeof(st->msg), "sent %s to %lu", what, pid);
    else if ((errno == EPERM || errno == EACCES) && critical)
        snprintf(st->msg, sizeof(st->msg), "%lu: protected system process (use Power off)", pid);
    else if (errno == EPERM || errno == EACCES)
        snprintf(st->msg, sizeof(st->msg), "%lu: permission denied (not your process)", pid);
    else
        snprintf(st->msg, sizeof(st->msg), "%lu: %s", pid, strerror(errno));
    refresh(st);
}

static int key(void *ctx, int k)
{
    struct state *st = (struct state *)ctx;
    int sel = selIndex(st);
    int page = st->listRows > 1 ? st->listRows - 1 : 1;
    int to = sel;

    if (st->confirmPid != 0) {
        unsigned long pid = st->confirmPid;

        st->confirmPid = 0;
        if (k == 'y' || k == 'Y')
            sendSignal(st, pid, SIGKILL, "SIGKILL");
        else
            snprintf(st->msg, sizeof(st->msg), "not killed");
        return 1;
    }
    st->msg[0] = '\0';

    switch (k) {
    case 'q': case 'Q': case NX64_KEY_ESC: case NX64_KEY_CTRL('c'):
        return 0;
    case NX64_KEY_UP:   to = sel - 1; break;
    case NX64_KEY_DOWN: to = sel + 1; break;
    case NX64_KEY_PGUP: to = sel - page; break;
    case NX64_KEY_PGDN: to = sel + page; break;
    case NX64_KEY_HOME: to = 0; break;
    case NX64_KEY_END:  to = st->count - 1; break;
    case 's':
        st->sort = st->sort == SORT_PID ? SORT_CPU : st->sort == SORT_CPU ? SORT_MEM : SORT_PID;
        g_sortKey = st->sort;
        qsort(st->rows, (size_t)st->count, sizeof(st->rows[0]), cmpQsort);
        fixSelection(st);
        return 1;
    case 'k':
        if (sel >= 0)
            sendSignal(st, st->rows[sel].p.pid, SIGTERM, "SIGTERM");
        return 1;
    case 'i':
        if (sel >= 0)
            sendSignal(st, st->rows[sel].p.pid, SIGINT, "SIGINT");
        return 1;
    case 'K':
        if (sel >= 0)
            st->confirmPid = st->rows[sel].p.pid;
        return 1;
    default:
        return 1;
    }
    if (st->count > 0) {
        if (to < 0)
            to = 0;
        if (to >= st->count)
            to = st->count - 1;
        st->selPid = st->rows[to].p.pid;
        st->selPos = to;
    }
    return 1;
}

static int tick(void *ctx)
{
    refresh((struct state *)ctx);
    return 1;
}

/* ---------------- 한 번 찍기 (-b) ---------------- */

static int batch(struct state *st)
{
    char a[24];
    char b[24];
    char name[64];
    int i;

    refresh(st);
    if (st->msg[0]) {
        fprintf(stderr, "ntop: %s\n", st->msg);
        return 1;
    }
    printf("%6s %6s %-10s S %7s %9s  NAME\n", "PID", "PPID", "USER", "MEM", "TIME");
    for (i = 0; i < st->count; i++) {
        const struct nx64_proc *p = &st->rows[i].p;

        fmtMem(p->mem_kib, a, sizeof(a));
        fmtTime(p->cpu_ticks, st->hz, b, sizeof(b));
        fmtName(p, name, sizeof(name));
        printf("%6lu %6lu %-10.10s %c %7s %9s  %s\n", p->pid, p->ppid, p->user[0] ? p->user : "-",
               stateChar(p->state), a, b, name);
    }
    return fflush(stdout) == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    static struct state st;
    struct nx64_app app;
    char v[16];
    long ms = 1000;

    memset(&st, 0, sizeof(st));
    st.hz = nx64_timer_hz();
    if (nx64_whoami(&st.me) < 0)
        strcpy(st.me.name, "?");
    if (nx64_setting_get("ntop", "sort", v, sizeof(v)) > 0)
        st.sort = strcmp(v, "cpu") == 0 ? SORT_CPU : strcmp(v, "mem") == 0 ? SORT_MEM : SORT_PID;
    if (argc == 2 && strcmp(argv[1], "-b") == 0)
        return batch(&st);
    if (argc > 1) {
        fputs("usage: ntop [-b]\n", stderr);
        return 2;
    }
    if (nx64_setting_get("ntop", "refresh_ms", v, sizeof(v)) > 0) {
        ms = strtol(v, NULL, 10);
        if (ms < 200 || ms > 10000)
            ms = 1000;
    }
    refresh(&st);
    st.selPid = st.count > 0 ? st.rows[0].p.pid : 0;

    app.draw = draw;
    app.key = key;
    app.tick = tick;
    app.tick_ms = (int)ms;
    if (nx64_tui_run(&app, &st) < 0) {
        fputs("ntop: no terminal\n", stderr);
        return 1;
    }
    return 0;
}
