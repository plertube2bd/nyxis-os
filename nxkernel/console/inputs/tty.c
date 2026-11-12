/*
 * tty.c - 터미널: 입력 줄 편집기 + 블로킹 읽기, 사용자 출력, 의사 터미널 (설계는 tty.h 참고)
 */

#include "console/inputs/tty.h"
#include "console/inputs/inputs_base.h"
#include "console/outputs/conout.h"
#include "console/outputs/outputs_base.h"
#include "console/outputs/ansi.h"
#include "nyx_abi.h"
#include "console/outputs/serial.h"
#include "kernel/process/waitq.h"
#include "kernel/process/process.h"
#include "kernel/process/schedule.h"
#include "kernel/process/signal.h"
#include "kernel/mm/kheap.h"
#include "kernel/syscall/uaccess.h"
#include "drivers/pic/pic.h"
#include "interrupt.h"
#include "lowlevel.h"
#include "memory.h"
#include "kernel/timer/pit/pit_base.h"
#include "kernel/error_handling/panic.h"

#define SERIAL_IRQ 4U
#define TTY_WRITE_CHUNK 64U
#define PTY_MAX 16U                 /* 동시에 있을 수 있는 pty 수 */

/* 입력 이스케이프 시퀀스 (방향키 등): ESC [ <숫자> <끝> 또는 ESC O <끝> */
enum { IN_GROUND, IN_ESC, IN_CSI };

/* ---------------- pty 객체 ---------------- */

struct pty {
    tty_t tty;                      /* 종속 끝 터미널 (첫 멤버) */
    char *out;                      /* 종속 끝 출력 (링, PTY_OUT_MAX) */
    u32 out_head;
    u32 out_count;
    waitq_t out_readers;            /* 주 끝 읽기가 기다린다 */
    waitq_t out_writers;            /* 종속 끝 쓰기가 공간을 기다린다 */
    u32 master_refs;
    u32 slave_refs;
    struct nx_winsize ws;
    bool in_use;
};

/* ---------------- 콘솔 ---------------- */

static char g_con_line[TTY_LINE_MAX];
static char g_con_ready[TTY_READY_MAX];
static char g_con_hist[TTY_HIST_MAX][TTY_LINE_MAX];
static tty_t g_console;
static volatile bool g_serial_rx = true;   /* dev:/com1 을 사용자 프로세스가 가져가면 끈다 */

static struct pty *g_ptys[PTY_MAX];        /* 살아 있는 pty (tty_process_terminated 가 훑는다) */

tty_t *tty_console(void)
{
    return &g_console;
}

static void tty_setup(tty_t *t, char *line, char *ready, char (*hist)[TTY_LINE_MAX])
{
    memset(t, 0, sizeof(*t));
    t->line = line;
    t->ready = ready;
    t->hist = hist;
    t->echo = true;
    t->in_state = IN_GROUND;
    waitq_init(&t->readers);
}

/* ---------------- 출력 (에코 포함) ---------------- */

/* pty 주 끝이 읽을 버퍼에 넣는다. 자리가 모자라면 넘치는 것은 버린다 (에코: 잠들 수 없는 경로) */
static void pty_out_push(struct pty *p, const char *s, u32 n)
{
    u32 i;

    if (p->master_refs == 0)
        return;                             /* 읽을 쪽이 없다 */
    for (i = 0; i < n && p->out_count < PTY_OUT_MAX; i++) {
        p->out[(p->out_head + p->out_count) % PTY_OUT_MAX] = s[i];
        p->out_count++;
    }
    if (i)
        (void)waitq_wake_all(&p->out_readers);
}

static void out_putc(tty_t *t, char c)
{
    if (!t->pty) {
        conout_putc(c);
        return;
    }
    if (c == '\n')
        pty_out_push(t->pty, "\r\n", 2);
    else
        pty_out_push(t->pty, &c, 1);
}

/* 커서를 n 칸 왼쪽으로. pty 는 '\b' (터미널 에뮬레이터가 줄 처음에서 윗줄 끝으로 넘긴다) */
static void out_back(tty_t *t, u32 n)
{
    if (!t->pty) {
        conout_cursor_back(n);
        return;
    }
    while (n-- > 0)
        pty_out_push(t->pty, "\b", 1);
}

/* ---------------- 입력 버퍼 ---------------- */

void tty_set_echo(tty_t *t, bool on)
{
    t->echo = on;
}

void tty_set_serial_rx(bool on)
{
    g_serial_rx = on;
    if (serial_available()) {
        if (on)
            serial_enable_rx_interrupt();
        else
            serial_disable_rx_interrupt();
    }
}

bool tty_hung_up(tty_t *t)
{
    return (t->pty && t->pty->master_refs == 0) ? true : false;
}

bool tty_input_ready(tty_t *t)
{
    return (t->ready_count > 0 || t->eof_pending > 0 || tty_hung_up(t)) ? true : false;
}

waitq_t *tty_waitq(tty_t *t)
{
    return &t->readers;
}

/* 줄을 읽을 수 있는 버퍼로 옮긴다. 공간이 모자라면 줄 전체를 버린다 (줄이 쪼개져 섞이지 않도록) */
static void commit_line(tty_t *t)
{
    u32 i;

    if (t->line_len > TTY_READY_MAX - t->ready_count) {
        t->line_len = 0;
        return;
    }

    for (i = 0; i < t->line_len; i++)
        t->ready[(t->ready_head + t->ready_count + i) % TTY_READY_MAX] = t->line[i];
    t->ready_count += t->line_len;
    t->line_len = 0;

    (void)waitq_wake_all(&t->readers);
}

/* ===================== */
/* 줄 편집 (커서 이동)     */
/* ===================== */

/*
 * 화면 표시 규칙: 줄의 글자 i 는 입력 시작 위치에서 i 칸 뒤에 보인다. 탭은 한 칸(공백)으로 보여서
 * 커서를 몇 칸 움직일지 늘 글자 수로 셀 수 있다 (실제 줄 버퍼에는 '\t' 그대로 들어간다).
 * 커서를 왼쪽으로는 out_back, 오른쪽으로는 그 자리의 글자를 다시 찍어서 옮긴다
 * (그러면 화면과 시리얼 모두 줄바꿈 경계를 자연스럽게 넘는다).
 */

static void echo_char(tty_t *t, char c)
{
    out_putc(t, c == '\t' ? ' ' : c);
}

/* 줄의 [from, line_len) 을 다시 찍고, extra 칸을 공백으로 지운 뒤 커서를 to 위치로 되돌린다 */
static void redraw_tail(tty_t *t, u32 from, u32 extra, u32 to)
{
    u32 i;

    for (i = from; i < t->line_len; i++)
        echo_char(t, t->line[i]);
    for (i = 0; i < extra; i++)
        out_putc(t, ' ');
    out_back(t, t->line_len + extra - to);
}

static void move_left(tty_t *t, u32 n)
{
    if (n > t->pos)
        n = t->pos;
    if (t->echo && n)
        out_back(t, n);
    t->pos -= n;
}

static void move_right(tty_t *t, u32 n)
{
    while (n-- > 0 && t->pos < t->line_len) {
        if (t->echo)
            echo_char(t, t->line[t->pos]);
        t->pos++;
    }
}

static void insert_char(tty_t *t, char c)
{
    u32 i;

    if (t->line_len >= TTY_LINE_MAX - 1U)
        return;
    for (i = t->line_len; i > t->pos; i--)
        t->line[i] = t->line[i - 1];
    t->line[t->pos] = c;
    t->line_len++;
    t->pos++;
    if (t->echo) {
        echo_char(t, c);
        if (t->pos < t->line_len)
            redraw_tail(t, t->pos, 0, t->pos);
    }
}

/* 커서 자리의 글자 하나 지우기 (Delete) */
static void delete_at_cursor(tty_t *t)
{
    u32 i;

    if (t->pos >= t->line_len)
        return;
    for (i = t->pos; i + 1U < t->line_len; i++)
        t->line[i] = t->line[i + 1U];
    t->line_len--;
    if (t->echo)
        redraw_tail(t, t->pos, 1, t->pos);
}

/* 커서 앞 글자 지우기. 줄 끝이면 화면에는 "\b \b" 와 같다 (줄바꿈 경계도 넘는다) */
static void backspace(tty_t *t)
{
    if (t->pos == 0)
        return;
    move_left(t, 1);
    delete_at_cursor(t);
}

/* 화면의 줄 전체를 지우고 새 내용으로 바꾼다 (Ctrl-U, ↑/↓) */
static void replace_line(tty_t *t, const char *text, u32 len)
{
    u32 old_len = t->line_len;
    u32 i;

    move_left(t, t->pos);
    if (len > TTY_LINE_MAX - 1U)
        len = TTY_LINE_MAX - 1U;
    for (i = 0; i < len; i++)
        t->line[i] = text[i];
    t->line_len = len;
    if (t->echo)
        redraw_tail(t, 0, old_len > len ? old_len - len : 0, len);
    t->pos = len;
}

static void hist_save(tty_t *t)
{
    u32 len = t->line_len;
    u32 i;

    if (!t->echo || len == 0)
        return;
    /* 바로 앞 기록과 같으면 다시 남기지 않는다 */
    if (t->hist_count > 0) {
        u32 last = (t->hist_next + TTY_HIST_MAX - 1U) % TTY_HIST_MAX;

        if (t->hist_len[last] == len) {
            for (i = 0; i < len && t->hist[last][i] == t->line[i]; i++)
                ;
            if (i == len)
                return;
        }
    }
    for (i = 0; i < len; i++)
        t->hist[t->hist_next][i] = t->line[i];
    t->hist_len[t->hist_next] = len;
    t->hist_next = (t->hist_next + 1U) % TTY_HIST_MAX;
    if (t->hist_count < TTY_HIST_MAX)
        t->hist_count++;
}

/* ↑ (dir = +1) / ↓ (dir = -1) */
static void hist_browse(tty_t *t, i32 dir)
{
    u32 k;

    if (!t->echo)
        return;
    if (dir > 0 && t->hist_browse >= t->hist_count)
        return;
    if (dir < 0 && t->hist_browse == 0)
        return;
    t->hist_browse = (u32)((i32)t->hist_browse + dir);
    if (t->hist_browse == 0) {
        replace_line(t, "", 0);
        return;
    }
    k = (t->hist_next + TTY_HIST_MAX - t->hist_browse) % TTY_HIST_MAX;
    replace_line(t, t->hist[k], t->hist_len[k]);
}

static void edit_key(tty_t *t, u32 key)
{
    switch (key) {
    case TTY_KEY_LEFT:   move_left(t, 1); break;
    case TTY_KEY_RIGHT:  move_right(t, 1); break;
    case TTY_KEY_HOME:   move_left(t, t->pos); break;
    case TTY_KEY_END:    move_right(t, t->line_len - t->pos); break;
    case TTY_KEY_DELETE: delete_at_cursor(t); break;
    case TTY_KEY_UP:     hist_browse(t, 1); break;
    case TTY_KEY_DOWN:   hist_browse(t, -1); break;
    default: break;
    }
}

/* 입력 상태를 모두 버린다 (모드 전환) */
static void flush_input(tty_t *t)
{
    t->line_len = 0;
    t->pos = 0;
    t->in_state = IN_GROUND;
    t->hist_browse = 0;
    t->ready_head = 0;
    t->ready_count = 0;
    t->eof_pending = 0;
}

/* 빈 줄 하나를 읽을 수 있게 넘긴다 (Ctrl-C 뒤 프롬프트를 다시 띄우도록) */
static void push_newline(tty_t *t)
{
    t->line_len = 0;
    t->line[t->line_len++] = '\n';
    commit_line(t);
}

/*
 * Ctrl-C / Ctrl-\ (cooked 모드): 입력을 버리고 포그라운드 그룹에 신호를 보낸다 (signal_post 는 IRQ 에서도
 * 안전하다: 비트를 켜고 잠든 프로세스를 깨울 뿐, 실제 처리는 각 프로세스가 사용자 모드로 돌아갈 때 한다).
 * 받을 프로세스가 없으면(셸이 프롬프트에서 기다리는 중) 빈 줄을 넘겨 새 프롬프트를 띄우게 한다.
 */
static void interrupt_key(tty_t *t, u32 sig)
{
    out_putc(t, '^');
    out_putc(t, sig == NX_SIGQUIT ? '\\' : 'C');
    out_putc(t, '\n');
    flush_input(t);
    if (t->fg_pgid == 0 || signal_post_group(t->fg_pgid, sig, NX_SI_KERNEL) == 0)
        push_newline(t);
}

static void edit_char(tty_t *t, char c)
{
    if (c == '\r')
        c = '\n';

    if (c == '\n') {
        move_right(t, t->line_len - t->pos);   /* 커서가 줄 중간이어도 줄 끝에서 줄을 바꾼다 */
        hist_save(t);
        t->hist_browse = 0;
        t->line[t->line_len++] = '\n';          /* line_len <= TTY_LINE_MAX-1 이 항상 유지되므로 자리 있음 */
        out_putc(t, '\n');                      /* 에코를 꺼도 줄바꿈은 보여 준다 (비밀번호 입력 후 다음 줄로) */
        commit_line(t);
        t->pos = 0;
    } else if (c == '\b' || c == 0x7F) {
        backspace(t);
    } else if (c == 0x03) {                     /* Ctrl-C */
        interrupt_key(t, NX_SIGINT);
    } else if (c == 0x1C) {                     /* Ctrl-\ */
        interrupt_key(t, NX_SIGQUIT);
    } else if (c == 0x15) {                     /* Ctrl-U: 줄 전체 지움 */
        replace_line(t, "", 0);
    } else if (c == 0x01) {                     /* Ctrl-A: 줄 처음 */
        edit_key(t, TTY_KEY_HOME);
    } else if (c == 0x05) {                     /* Ctrl-E: 줄 끝 */
        edit_key(t, TTY_KEY_END);
    } else if (c == 0x04) {                     /* Ctrl-D */
        if (t->line_len == 0) {
            t->eof_pending++;
            (void)waitq_wake_all(&t->readers);
        } else {
            move_right(t, t->line_len - t->pos);
            commit_line(t);
            t->pos = 0;
        }
    } else if (c == '\t' || ((u8)c >= 0x20 && (u8)c < 0x7F)) {
        insert_char(t, c);
    }
}

/* raw 모드: 바이트들을 읽을 수 있는 버퍼에 바로 넣는다. 자리가 모자라면 통째로 버린다 (키 시퀀스가 잘리지 않도록) */
static void raw_push(tty_t *t, const char *s, u32 n)
{
    u32 i;

    if (n > TTY_READY_MAX - t->ready_count)
        return;
    for (i = 0; i < n; i++)
        t->ready[(t->ready_head + t->ready_count + i) % TTY_READY_MAX] = s[i];
    t->ready_count += n;
    (void)waitq_wake_all(&t->readers);
}

static void raw_key(tty_t *t, u32 key)
{
    switch (key) {
    case TTY_KEY_UP:     raw_push(t, "\033[A", 3); break;
    case TTY_KEY_DOWN:   raw_push(t, "\033[B", 3); break;
    case TTY_KEY_RIGHT:  raw_push(t, "\033[C", 3); break;
    case TTY_KEY_LEFT:   raw_push(t, "\033[D", 3); break;
    case TTY_KEY_HOME:   raw_push(t, "\033[H", 3); break;
    case TTY_KEY_END:    raw_push(t, "\033[F", 3); break;
    case TTY_KEY_DELETE: raw_push(t, "\033[3~", 4); break;
    case TTY_KEY_PGUP:   raw_push(t, "\033[5~", 4); break;
    case TTY_KEY_PGDN:   raw_push(t, "\033[6~", 4); break;
    default: break;
    }
}

void tty_input_key(u32 key)
{
    u64 flags = irq_save();
    tty_t *t = &g_console;

    if (t->raw) {
        raw_key(t, key);
    } else {
        t->in_state = IN_GROUND;
        edit_key(t, key);
    }
    irq_restore(flags);
}

/* 입력 바이트 하나 (인터럽트를 막은 상태에서) */
static void input_byte(tty_t *t, char c)
{
    u8 uc = (u8)c;

    if (t->raw) {
        /* raw: 그대로 넘긴다. 키보드의 Enter('\n')는 터미널처럼 '\r' 로 (pty 는 주 끝이 보낸 그대로) */
        char b = (c == '\n' && !t->pty) ? '\r' : c;

        raw_push(t, &b, 1);
        return;
    }

    switch (t->in_state) {
    case IN_ESC:
        if (uc == '[' || uc == 'O') {
            t->in_state = IN_CSI;
            t->in_num = 0;
            break;
        }
        t->in_state = IN_GROUND;            /* ESC 다음이 시퀀스가 아니면 ESC 는 버리고 이 글자는 평소대로 */
        edit_char(t, c);
        break;
    case IN_CSI:
        if (uc >= '0' && uc <= '9') {
            if (t->in_num < 1000U)
                t->in_num = t->in_num * 10U + (u32)(uc - '0');
            break;
        }
        if (uc == ';') {
            t->in_num = 0;                  /* 수정자(Shift 등)는 무시: 첫 숫자만 본다 */
            break;
        }
        t->in_state = IN_GROUND;
        switch (uc) {
        case 'A': edit_key(t, TTY_KEY_UP); break;
        case 'B': edit_key(t, TTY_KEY_DOWN); break;
        case 'C': edit_key(t, TTY_KEY_RIGHT); break;
        case 'D': edit_key(t, TTY_KEY_LEFT); break;
        case 'H': edit_key(t, TTY_KEY_HOME); break;
        case 'F': edit_key(t, TTY_KEY_END); break;
        case '~':                           /* VT: 1/7 Home, 4/8 End, 3 Delete */
            if (t->in_num == 1U || t->in_num == 7U)
                edit_key(t, TTY_KEY_HOME);
            else if (t->in_num == 4U || t->in_num == 8U)
                edit_key(t, TTY_KEY_END);
            else if (t->in_num == 3U)
                edit_key(t, TTY_KEY_DELETE);
            break;
        default:
            break;                          /* 모르는 시퀀스는 통째로 버린다 */
        }
        break;
    default:
        if (uc == 0x1B)
            t->in_state = IN_ESC;
        else
            edit_char(t, c);
        break;
    }
}

void tty_input_char(char c)
{
    u64 flags = irq_save();

    input_byte(&g_console, c);
    irq_restore(flags);
}

void tty_set_foreground(tty_t *t, u32 pgid)
{
    u64 flags = irq_save();

    t->fg_pgid = pgid;
    irq_restore(flags);
}

u32 tty_get_foreground(tty_t *t)
{
    return t->fg_pgid;
}

void tty_set_raw(tty_t *t, bool on, u32 owner_pid)
{
    u64 flags = irq_save();

    flush_input(t);
    t->raw = on;
    t->raw_owner = on ? owner_pid : 0;
    irq_restore(flags);
}

static void raw_owner_gone(tty_t *t, u32 pid)
{
    if (!t->raw || t->raw_owner != pid)
        return;
    flush_input(t);
    t->raw = false;
    t->raw_owner = 0;
    if (t->pty) {
        /* 화면을 그리다 죽었을 수 있다: 터미널 에뮬레이터에게 색/커서/주 화면을 되돌리게 한다 */
        static const char reset[] = "\033[0m\033[?25h\033[?1049l";

        pty_out_push(t->pty, reset, (u32)(sizeof(reset) - 1U));
    } else {
        ansi_reset();                       /* 색 기본값 */
        conout_set_cursor_visible(true);    /* 화면을 그리다 죽었으면 커서가 숨겨져 있을 수 있다 */
    }
}

void tty_process_terminated(u32 pid)
{
    u64 flags = irq_save();
    u32 i;

    raw_owner_gone(&g_console, pid);
    for (i = 0; i < PTY_MAX; i++) {
        if (g_ptys[i])
            raw_owner_gone(&g_ptys[i]->tty, pid);
    }
    irq_restore(flags);
}

void tty_get_winsize(tty_t *t, struct nx_winsize *out)
{
    u32 rows;
    u32 cols;

    if (t->pty) {
        *out = t->pty->ws;
        return;
    }
    conout_get_size(&rows, &cols);
    if (rows == 0 || cols == 0) {
        out->rows = 24;                     /* 시리얼만: 흔한 터미널 크기로 가정 */
        out->cols = 80;
        out->xpixel = 0;
        out->ypixel = 0;
        return;
    }
    out->rows = (u16)rows;
    out->cols = (u16)cols;
    out->xpixel = (u16)(cols * font_cell_width());
    out->ypixel = (u16)(rows * font_cell_height());
}

/* ---------------- 사용자 출력 ---------------- */

/* 콘솔: 터미널 해석기(ansi.c)를 거쳐 출력 장치로. 입력 에코(IRQ)와 글자가 섞여 커서 상태가 깨지지 않도록 막는다 */
static void console_write(const char *buf, usize len)
{
    usize i;
    u64 flags;

    /*
     * 인터럽트는 TTY_WRITE_CHUNK 바이트씩만 막는다. 큰 쓰기(편집기의 화면 전체 다시 그리기 등)를 통째로 막으면
     * 그동안 키보드 컨트롤러의 버퍼가 넘쳐 키 입력이 사라진다. 해석기 상태는 바이트 사이에 끊겨도 이어진다.
     */
    conout_batch_begin();                   /* 커서는 다 쓴 뒤 한 번만 다시 그린다 */
    for (i = 0; i < len; ) {
        usize end = (len - i > TTY_WRITE_CHUNK) ? i + TTY_WRITE_CHUNK : len;

        flags = irq_save();
        for (; i < end; i++)
            ansi_feed((u8)buf[i]);          /* 글자 + 허용된 이스케이프 시퀀스만 (ansi.h) */
        irq_restore(flags);
    }
    conout_batch_end();
}

/* pty 종속 끝 쓰기: 주 끝이 읽을 버퍼로. 가득 차면 잠든다 */
static Nstatus pty_slave_write(struct pty *p, const char *buf, usize len, usize *out_done)
{
    u64 flags = irq_save();
    usize done = 0;
    Nstatus status = NSTATUS_OK;

    while (done < len) {
        if (p->master_refs == 0) {
            status = Ndisconnected;
            break;
        }
        if (p->out_count == PTY_OUT_MAX) {
            status = waitq_sleep_locked(&p->out_writers, 0);
            if (NSTATUS_IS_ERR(status))
                break;                      /* 신호: 쓴 만큼만 */
            continue;
        }
        while (done < len && p->out_count < PTY_OUT_MAX) {
            p->out[(p->out_head + p->out_count) % PTY_OUT_MAX] = buf[done++];
            p->out_count++;
        }
        (void)waitq_wake_all(&p->out_readers);
    }
    irq_restore(flags);
    *out_done = done;
    if (done > 0)
        return NSTATUS_OK;                  /* 일부라도 썼으면 짧은 쓰기 */
    return status;
}

Nstatus tty_write(tty_t *t, const char *buf, usize len, usize *out_done)
{
    *out_done = 0;
    if (!buf)
        return NinvalidArg;
    if (t->pty)
        return pty_slave_write(t->pty, buf, len, out_done);
    console_write(buf, len);
    *out_done = len;
    return NSTATUS_OK;
}

Nstatus tty_read(tty_t *t, char *buf, usize len, usize *out_got)
{
    u64 flags;
    usize n = 0;

    if (!buf || !out_got)
        return NinvalidArg;
    *out_got = 0;
    if (len == 0)
        return NSTATUS_OK;

    flags = irq_save();

    while (t->ready_count == 0 && t->eof_pending == 0 && !tty_hung_up(t)) {
        Nstatus status = waitq_sleep_locked(&t->readers, 0);

        if (NSTATUS_IS_ERR(status)) {
            irq_restore(flags);
            return status;
        }
    }

    if (t->ready_count > 0) {
        /* 최대 한 줄: '\n' 을 복사하면 멈춘다 */
        while (n < len && t->ready_count > 0) {
            char c = t->ready[t->ready_head];

            buf[n++] = c;
            t->ready_head = (t->ready_head + 1U) % TTY_READY_MAX;
            t->ready_count--;
            if (c == '\n' && !t->raw)
                break;
        }
    } else if (t->eof_pending > 0) {
        t->eof_pending--;                   /* EOF: 0 바이트 */
    }
    /* 끊긴 pty: 남은 입력을 다 읽으면 계속 0 (EOF) */

    irq_restore(flags);
    *out_got = n;
    return NSTATUS_OK;
}

/* ---------------- 의사 터미널 ---------------- */

Nstatus pty_create(struct pty **out)
{
    struct pty *p;
    char *buf;
    char (*hist)[TTY_LINE_MAX];
    char *out_buf;
    u64 flags;
    u32 slot;

    if (!out)
        return NinvalidArg;
    p = (struct pty *)kzalloc(sizeof(*p));
    buf = (char *)kmalloc(TTY_LINE_MAX + TTY_READY_MAX);
    hist = (char (*)[TTY_LINE_MAX])kpage_alloc();       /* TTY_HIST_MAX * TTY_LINE_MAX = 4096 */
    out_buf = (char *)kpage_alloc();                    /* PTY_OUT_MAX = 4096 */
    if (!p || !buf || !hist || !out_buf)
        goto fail;

    tty_setup(&p->tty, buf, buf + TTY_LINE_MAX, hist);
    p->tty.pty = p;
    p->out = out_buf;
    waitq_init(&p->out_readers);
    waitq_init(&p->out_writers);
    p->master_refs = 1;
    p->slave_refs = 1;
    p->ws.rows = 24;
    p->ws.cols = 80;

    flags = irq_save();
    for (slot = 0; slot < PTY_MAX; slot++) {
        if (!g_ptys[slot]) {
            g_ptys[slot] = p;
            break;
        }
    }
    irq_restore(flags);
    if (slot == PTY_MAX)
        goto fail;
    p->in_use = true;
    *out = p;
    return NSTATUS_OK;

fail:
    if (out_buf)
        kpage_free(out_buf);
    if (hist)
        kpage_free(hist);
    if (buf)
        kfree(buf);
    if (p)
        kfree(p);
    return NoutOfMemory;
}

tty_t *pty_tty(struct pty *p)
{
    return &p->tty;
}

/* 양쪽 참조가 모두 0 이면 해제 (인터럽트를 막은 상태에서) */
static void pty_maybe_free(struct pty *p)
{
    u32 i;

    if (p->master_refs || p->slave_refs)
        return;
    for (i = 0; i < PTY_MAX; i++) {
        if (g_ptys[i] == p)
            g_ptys[i] = nNULL;
    }
    p->in_use = false;
    kpage_free(p->out);
    kpage_free(p->tty.hist);
    kfree(p->tty.line);                     /* line 과 ready 는 한 덩어리 */
    kfree(p);
}

void pty_master_retain(struct pty *p)
{
    u64 flags = irq_save();

    p->master_refs++;
    irq_restore(flags);
}

void pty_master_release(struct pty *p)
{
    u64 flags = irq_save();

    if (p->master_refs > 0 && --p->master_refs == 0) {
        /* 끊김: 종속 끝 읽기/쓰기를 깨우고, 이 터미널을 쓰는 프로세스들에게 SIGHUP */
        p->out_count = 0;
        (void)waitq_wake_all(&p->tty.readers);
        (void)waitq_wake_all(&p->out_writers);
        if (p->tty.fg_pgid)
            (void)signal_post_group(p->tty.fg_pgid, NX_SIGHUP, NX_SI_KERNEL);
        process_hangup_ctty(&p->tty);
    }
    pty_maybe_free(p);
    irq_restore(flags);
}

void tty_retain(tty_t *t)
{
    u64 flags;

    if (!t || !t->pty)
        return;
    flags = irq_save();
    t->pty->slave_refs++;
    irq_restore(flags);
}

void tty_release(tty_t *t)
{
    u64 flags;
    struct pty *p;

    if (!t || !t->pty)
        return;
    p = t->pty;
    flags = irq_save();
    if (p->slave_refs > 0 && --p->slave_refs == 0)
        (void)waitq_wake_all(&p->out_readers);     /* 주 끝 읽기에 EOF */
    pty_maybe_free(p);
    irq_restore(flags);
}

i64 pty_master_read_user(struct pty *p, u64 ubuf, u64 len)
{
    u8 chunk[256];
    u64 flags = irq_save();
    usize n = 0;
    Nstatus status;

    while (p->out_count == 0) {
        if (p->slave_refs == 0) {
            irq_restore(flags);
            return 0;                       /* EOF */
        }
        status = waitq_sleep_locked(&p->out_readers, 0);
        if (NSTATUS_IS_ERR(status)) {
            irq_restore(flags);
            return (i64)status;
        }
    }
    while (n < len && n < sizeof(chunk) && p->out_count > 0) {
        chunk[n++] = (u8)p->out[p->out_head];
        p->out_head = (p->out_head + 1U) % PTY_OUT_MAX;
        p->out_count--;
    }
    (void)waitq_wake_all(&p->out_writers);
    irq_restore(flags);

    status = copy_to_user(ubuf, chunk, n);  /* 실패하면 이 바이트들은 버려진다 (사용자 버퍼가 잘못됨) */
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    return (i64)n;
}

i64 pty_master_write_user(struct pty *p, u64 ubuf, u64 len)
{
    u8 chunk[256];
    u64 total = 0;

    while (total < len) {
        usize n = (usize)((len - total) < sizeof(chunk) ? (len - total) : sizeof(chunk));
        Nstatus status = copy_from_user(chunk, ubuf + total, n);
        u64 flags;
        usize i;

        if (NSTATUS_IS_ERR(status))
            return total ? (i64)total : (i64)status;
        flags = irq_save();
        for (i = 0; i < n; i++)
            input_byte(&p->tty, (char)chunk[i]);
        irq_restore(flags);
        total += n;
    }
    return (i64)total;
}

void pty_set_winsize(struct pty *p, const struct nx_winsize *ws)
{
    u64 flags = irq_save();
    bool changed = p->ws.rows != ws->rows || p->ws.cols != ws->cols;

    p->ws = *ws;
    if (changed && p->tty.fg_pgid)
        (void)signal_post_group(p->tty.fg_pgid, NX_SIGWINCH, NX_SI_KERNEL);
    irq_restore(flags);
}

u32 pty_master_poll_bits(struct pty *p)
{
    u32 bits = NX_POLL_OUT;

    if (p->out_count > 0)
        bits |= NX_POLL_IN;
    if (p->slave_refs == 0)
        bits |= NX_POLL_IN | NX_POLL_HUP;
    return bits;
}

waitq_t *pty_master_waitq(struct pty *p)
{
    return &p->out_readers;
}

/* ---------------- 시리얼 / 초기화 ---------------- */

/*
 * 시리얼 SysRq (Linux 와 같다): BREAK 를 보내고 5초 안에 글자 하나 -> sysrq_key (c = 강제 패닉).
 * BREAK 는 0x00 바이트와 함께 오므로 그 바이트는 버린다. QEMU -serial mon:stdio 에서는 Ctrl-A b 가 BREAK 다.
 */
static u64 g_sysrq_armed_until;

static void serial_irq_handler(struct trap_frame *frame)
{
    u32 guard = 0;
    char c;
    bool brk;

    if (!g_serial_rx)
        return;                            /* COM1 은 지금 사용자 프로세스 소유 */
    while (guard++ < 64U && serial_rx_byte(&c, &brk)) {
        if (brk) {
            g_sysrq_armed_until = timer_get_tick() + 5UL * timer_get_hz();
            continue;
        }
        if (g_sysrq_armed_until != 0) {
            bool armed = timer_get_tick() <= g_sysrq_armed_until;

            g_sysrq_armed_until = 0;
            if (armed && c >= 'a' && c <= 'z' && sysrq_key(c, frame))
                continue;
        }
        tty_input_char(c);
    }
}

void tty_init(void)
{
    tty_setup(&g_console, g_con_line, g_con_ready, g_con_hist);

    keyboard_init();

    if (serial_available()) {
        if (irq_register_handler((u8)SERIAL_IRQ, serial_irq_handler) == NSTATUS_OK) {
            serial_enable_rx_interrupt();
            pic_clear_mask((u8)SERIAL_IRQ);
        }
    }
}
