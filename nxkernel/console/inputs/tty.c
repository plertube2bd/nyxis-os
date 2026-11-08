/*
 * tty.c - 콘솔 터미널: 입력 줄 편집기 + 블로킹 읽기, 사용자 출력 (설계는 tty.h 참고)
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
#include "drivers/pic/pic.h"
#include "interrupt.h"
#include "lowlevel.h"

#define SERIAL_IRQ 4U
#define TTY_WRITE_CHUNK 64U

static char g_line[TTY_LINE_MAX];          /* 편집 중인 줄 */
static u32 g_line_len = 0;

static char g_ready[TTY_READY_MAX];        /* 완성되어 읽을 수 있는 바이트 (링 버퍼) */
static u32 g_ready_head = 0;               /* 다음에 읽을 위치 */
static u32 g_ready_count = 0;
static u32 g_eof_pending = 0;              /* 아직 소비되지 않은 Ctrl-D(EOF) 수 */

static waitq_t g_readers;
static volatile bool g_echo = true;        /* NX_DEVCMD_TTY_ECHO 로 끈다 (비밀번호 입력) */
static volatile bool g_serial_rx = true;   /* dev:/com1 을 사용자 프로세스가 가져가면 끈다 */
static volatile bool g_raw = false;        /* NX_DEVCMD_TTY_MODE */
static u32 g_raw_owner = 0;                /* raw 를 켠 프로세스 */
static u32 g_fg_pgid = 0;                  /* 포그라운드 프로세스 그룹 (0 = 없음) */

void tty_set_echo(bool on)
{
    g_echo = on;
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

bool tty_input_ready(void)
{
    return (g_ready_count > 0 || g_eof_pending > 0) ? true : false;
}

waitq_t *tty_waitq(void)
{
    return &g_readers;
}

/* 줄을 읽을 수 있는 버퍼로 옮긴다. 공간이 모자라면 줄 전체를 버린다 (줄이 쪼개져 섞이지 않도록) */
static void commit_line(void)
{
    u32 i;

    if (g_line_len > TTY_READY_MAX - g_ready_count) {
        g_line_len = 0;
        return;
    }

    for (i = 0; i < g_line_len; i++)
        g_ready[(g_ready_head + g_ready_count + i) % TTY_READY_MAX] = g_line[i];
    g_ready_count += g_line_len;
    g_line_len = 0;

    (void)waitq_wake_all(&g_readers);
}

/* ===================== */
/* 줄 편집 (커서 이동)     */
/* ===================== */

/*
 * 화면 표시 규칙: 줄의 글자 i 는 입력 시작 위치에서 i 칸 뒤에 보인다. 탭은 한 칸(공백)으로 보여서
 * 커서를 몇 칸 움직일지 늘 글자 수로 셀 수 있다 (실제 줄 버퍼에는 '\t' 그대로 들어간다).
 * 커서를 왼쪽으로는 conout_cursor_back, 오른쪽으로는 그 자리의 글자를 다시 찍어서 옮긴다
 * (그러면 프레임버퍼와 시리얼 모두 줄바꿈 경계를 자연스럽게 넘는다).
 */
static u32 g_pos = 0;                      /* 줄 안의 커서 위치 (0..g_line_len) */

/* 입력 기록 (↑/↓). 에코가 꺼진 입력(비밀번호)은 남기지 않는다 */
#define TTY_HIST_MAX 16U
static char g_hist[TTY_HIST_MAX][TTY_LINE_MAX];
static u32  g_hist_len[TTY_HIST_MAX];
static u32  g_hist_count = 0;              /* 저장된 줄 수 (<= TTY_HIST_MAX) */
static u32  g_hist_next = 0;               /* 다음에 쓸 칸 (링) */
static u32  g_hist_browse = 0;             /* 0 = 편집 중인 줄, k = k 번째 이전 기록 */

/* 시리얼 입력의 이스케이프 시퀀스 (방향키 등): ESC [ <숫자> <끝> 또는 ESC O <끝> */
enum { IN_GROUND, IN_ESC, IN_CSI };
static u32 g_in_state = IN_GROUND;
static u32 g_in_num = 0;

static void echo_char(char c)
{
    conout_putc(c == '\t' ? ' ' : c);
}

/* 줄의 [from, g_line_len) 을 다시 찍고, extra 칸을 공백으로 지운 뒤 커서를 to 위치로 되돌린다 */
static void redraw_tail(u32 from, u32 extra, u32 to)
{
    u32 i;

    for (i = from; i < g_line_len; i++)
        echo_char(g_line[i]);
    for (i = 0; i < extra; i++)
        conout_putc(' ');
    conout_cursor_back(g_line_len + extra - to);
}

static void move_left(u32 n)
{
    if (n > g_pos)
        n = g_pos;
    if (g_echo && n)
        conout_cursor_back(n);
    g_pos -= n;
}

static void move_right(u32 n)
{
    while (n-- > 0 && g_pos < g_line_len) {
        if (g_echo)
            echo_char(g_line[g_pos]);
        g_pos++;
    }
}

static void insert_char(char c)
{
    u32 i;

    if (g_line_len >= TTY_LINE_MAX - 1U)
        return;
    for (i = g_line_len; i > g_pos; i--)
        g_line[i] = g_line[i - 1];
    g_line[g_pos] = c;
    g_line_len++;
    g_pos++;
    if (g_echo) {
        echo_char(c);
        if (g_pos < g_line_len)
            redraw_tail(g_pos, 0, g_pos);
    }
}

/* 커서 자리의 글자 하나 지우기 (Delete) */
static void delete_at_cursor(void)
{
    u32 i;

    if (g_pos >= g_line_len)
        return;
    for (i = g_pos; i + 1U < g_line_len; i++)
        g_line[i] = g_line[i + 1U];
    g_line_len--;
    if (g_echo)
        redraw_tail(g_pos, 1, g_pos);
}

/* 커서 앞 글자 지우기. 줄 끝이면 화면에는 "\b \b" 와 같다 (줄바꿈 경계도 넘는다) */
static void backspace(void)
{
    if (g_pos == 0)
        return;
    move_left(1);
    delete_at_cursor();
}

/* 화면의 줄 전체를 지우고 새 내용으로 바꾼다 (Ctrl-U, ↑/↓) */
static void replace_line(const char *text, u32 len)
{
    u32 old_len = g_line_len;
    u32 i;

    move_left(g_pos);
    if (len > TTY_LINE_MAX - 1U)
        len = TTY_LINE_MAX - 1U;
    for (i = 0; i < len; i++)
        g_line[i] = text[i];
    g_line_len = len;
    if (g_echo)
        redraw_tail(0, old_len > len ? old_len - len : 0, len);
    g_pos = len;
}

static void hist_save(void)
{
    u32 len = g_line_len;
    u32 i;

    if (!g_echo || len == 0)
        return;
    /* 바로 앞 기록과 같으면 다시 남기지 않는다 */
    if (g_hist_count > 0) {
        u32 last = (g_hist_next + TTY_HIST_MAX - 1U) % TTY_HIST_MAX;

        if (g_hist_len[last] == len) {
            for (i = 0; i < len && g_hist[last][i] == g_line[i]; i++)
                ;
            if (i == len)
                return;
        }
    }
    for (i = 0; i < len; i++)
        g_hist[g_hist_next][i] = g_line[i];
    g_hist_len[g_hist_next] = len;
    g_hist_next = (g_hist_next + 1U) % TTY_HIST_MAX;
    if (g_hist_count < TTY_HIST_MAX)
        g_hist_count++;
}

/* ↑ (dir = +1) / ↓ (dir = -1) */
static void hist_browse(i32 dir)
{
    u32 k;

    if (!g_echo)
        return;
    if (dir > 0 && g_hist_browse >= g_hist_count)
        return;
    if (dir < 0 && g_hist_browse == 0)
        return;
    g_hist_browse = (u32)((i32)g_hist_browse + dir);
    if (g_hist_browse == 0) {
        replace_line("", 0);
        return;
    }
    k = (g_hist_next + TTY_HIST_MAX - g_hist_browse) % TTY_HIST_MAX;
    replace_line(g_hist[k], g_hist_len[k]);
}

static void edit_key(u32 key)
{
    switch (key) {
    case TTY_KEY_LEFT:   move_left(1); break;
    case TTY_KEY_RIGHT:  move_right(1); break;
    case TTY_KEY_HOME:   move_left(g_pos); break;
    case TTY_KEY_END:    move_right(g_line_len - g_pos); break;
    case TTY_KEY_DELETE: delete_at_cursor(); break;
    case TTY_KEY_UP:     hist_browse(1); break;
    case TTY_KEY_DOWN:   hist_browse(-1); break;
    default: break;
    }
}

static void flush_input(void);
static void interrupt_key(u32 sig);

static void edit_char(char c)
{
    if (c == '\r')
        c = '\n';

    if (c == '\n') {
        move_right(g_line_len - g_pos);    /* 커서가 줄 중간이어도 줄 끝에서 줄을 바꾼다 */
        hist_save();
        g_hist_browse = 0;
        g_line[g_line_len++] = '\n';       /* g_line_len <= TTY_LINE_MAX-1 이 항상 유지되므로 자리 있음 */
        conout_putc('\n');               /* 에코를 꺼도 줄바꿈은 보여 준다 (비밀번호 입력 후 다음 줄로) */
        commit_line();
        g_pos = 0;
    } else if (c == '\b' || c == 0x7F) {
        backspace();
    } else if (c == 0x03) {                /* Ctrl-C */
        interrupt_key(NX_SIGINT);
    } else if (c == 0x1C) {                /* Ctrl-\ */
        interrupt_key(NX_SIGQUIT);
    } else if (c == 0x15) {                /* Ctrl-U: 줄 전체 지움 */
        replace_line("", 0);
    } else if (c == 0x01) {                /* Ctrl-A: 줄 처음 */
        edit_key(TTY_KEY_HOME);
    } else if (c == 0x05) {                /* Ctrl-E: 줄 끝 */
        edit_key(TTY_KEY_END);
    } else if (c == 0x04) {                /* Ctrl-D */
        if (g_line_len == 0) {
            g_eof_pending++;
            (void)waitq_wake_all(&g_readers);
        } else {
            move_right(g_line_len - g_pos);
            commit_line();
            g_pos = 0;
        }
    } else if (c == '\t' || ((u8)c >= 0x20 && (u8)c < 0x7F)) {
        insert_char(c);
    }
}

/* raw 모드: 바이트들을 읽을 수 있는 버퍼에 바로 넣는다. 자리가 모자라면 통째로 버린다 (키 시퀀스가 잘리지 않도록) */
static void raw_push(const char *s, u32 n)
{
    u32 i;

    if (n > TTY_READY_MAX - g_ready_count)
        return;
    for (i = 0; i < n; i++)
        g_ready[(g_ready_head + g_ready_count + i) % TTY_READY_MAX] = s[i];
    g_ready_count += n;
    (void)waitq_wake_all(&g_readers);
}

static void raw_key(u32 key)
{
    switch (key) {
    case TTY_KEY_UP:     raw_push("\033[A", 3); break;
    case TTY_KEY_DOWN:   raw_push("\033[B", 3); break;
    case TTY_KEY_RIGHT:  raw_push("\033[C", 3); break;
    case TTY_KEY_LEFT:   raw_push("\033[D", 3); break;
    case TTY_KEY_HOME:   raw_push("\033[H", 3); break;
    case TTY_KEY_END:    raw_push("\033[F", 3); break;
    case TTY_KEY_DELETE: raw_push("\033[3~", 4); break;
    case TTY_KEY_PGUP:   raw_push("\033[5~", 4); break;
    case TTY_KEY_PGDN:   raw_push("\033[6~", 4); break;
    default: break;
    }
}

void tty_input_key(u32 key)
{
    u64 flags = irq_save();

    if (g_raw) {
        raw_key(key);
    } else {
        g_in_state = IN_GROUND;
        edit_key(key);
    }
    irq_restore(flags);
}

/* 입력 상태를 모두 버린다 (모드 전환) */
static void flush_input(void)
{
    g_line_len = 0;
    g_pos = 0;
    g_in_state = IN_GROUND;
    g_hist_browse = 0;
    g_ready_head = 0;
    g_ready_count = 0;
    g_eof_pending = 0;
}

/* 빈 줄 하나를 읽을 수 있게 넘긴다 (Ctrl-C 뒤 프롬프트를 다시 띄우도록) */
static void push_newline(void)
{
    g_line_len = 0;
    g_line[g_line_len++] = '\n';
    commit_line();
}

/*
 * Ctrl-C / Ctrl-\ (cooked 모드, IRQ 안): 입력을 버리고 포그라운드 그룹에 신호를 보낸다 (signal_post 는 IRQ 에서도
 * 안전하다: 비트를 켜고 잠든 프로세스를 깨울 뿐, 실제 처리는 각 프로세스가 사용자 모드로 돌아갈 때 한다).
 * 받을 프로세스가 없으면(셸이 프롬프트에서 기다리는 중) 빈 줄을 넘겨 새 프롬프트를 띄우게 한다.
 */
static void interrupt_key(u32 sig)
{
    conout_putc('^');
    conout_putc(sig == NX_SIGQUIT ? '\\' : 'C');
    conout_putc('\n');
    flush_input();
    if (g_fg_pgid == 0 || signal_post_group(g_fg_pgid, sig, NX_SI_KERNEL) == 0)
        push_newline();
}

void tty_set_foreground(u32 pgid)
{
    u64 flags = irq_save();

    g_fg_pgid = pgid;
    irq_restore(flags);
}

u32 tty_get_foreground(void)
{
    return g_fg_pgid;
}

void tty_set_raw(bool on, u32 owner_pid)
{
    u64 flags = irq_save();

    flush_input();
    g_raw = on;
    g_raw_owner = on ? owner_pid : 0;
    irq_restore(flags);
}

void tty_process_terminated(u32 pid)
{
    u64 flags = irq_save();

    if (g_raw && g_raw_owner == pid) {
        flush_input();
        g_raw = false;
        g_raw_owner = 0;
        ansi_reset();                       /* 색 기본값 */
        conout_set_cursor_visible(true);    /* 화면을 그리다 죽었으면 커서가 숨겨져 있을 수 있다 */
    }
    irq_restore(flags);
}

void tty_get_winsize(struct nx_winsize *out)
{
    u32 rows;
    u32 cols;

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

void tty_input_char(char c)
{
    u64 flags = irq_save();
    u8 uc = (u8)c;

    if (g_raw) {
        /* raw: 그대로 넘긴다. 키보드의 Enter('\n')는 터미널처럼 '\r' 로 */
        char b = (c == '\n') ? '\r' : c;

        raw_push(&b, 1);
        irq_restore(flags);
        return;
    }

    switch (g_in_state) {
    case IN_ESC:
        if (uc == '[' || uc == 'O') {
            g_in_state = IN_CSI;
            g_in_num = 0;
            break;
        }
        g_in_state = IN_GROUND;            /* ESC 다음이 시퀀스가 아니면 ESC 는 버리고 이 글자는 평소대로 */
        edit_char(c);
        break;
    case IN_CSI:
        if (uc >= '0' && uc <= '9') {
            if (g_in_num < 1000U)
                g_in_num = g_in_num * 10U + (u32)(uc - '0');
            break;
        }
        if (uc == ';')
            break;                         /* 수정자(Shift 등)는 무시 */
        g_in_state = IN_GROUND;
        switch (uc) {
        case 'A': edit_key(TTY_KEY_UP); break;
        case 'B': edit_key(TTY_KEY_DOWN); break;
        case 'C': edit_key(TTY_KEY_RIGHT); break;
        case 'D': edit_key(TTY_KEY_LEFT); break;
        case 'H': edit_key(TTY_KEY_HOME); break;
        case 'F': edit_key(TTY_KEY_END); break;
        case '~':                          /* VT: 1/7 Home, 4/8 End, 3 Delete */
            if (g_in_num == 1U || g_in_num == 7U)
                edit_key(TTY_KEY_HOME);
            else if (g_in_num == 4U || g_in_num == 8U)
                edit_key(TTY_KEY_END);
            else if (g_in_num == 3U)
                edit_key(TTY_KEY_DELETE);
            break;
        default:
            break;                         /* 모르는 시퀀스는 통째로 버린다 */
        }
        break;
    default:
        if (uc == 0x1B)
            g_in_state = IN_ESC;
        else
            edit_char(c);
        break;
    }

    irq_restore(flags);
}

/* 사용자 출력: 터미널 해석기(ansi.c)를 거쳐 출력 장치로. 입력 에코(IRQ)와 글자가 섞여 커서 상태가 깨지지 않도록 막는다 */
void tty_write(const char *buf, usize len)
{
    usize i;
    u64 flags;

    if (!buf)
        return;

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

Nstatus tty_read(char *buf, usize len, usize *out_got)
{
    u64 flags;
    usize n = 0;

    if (!buf || !out_got)
        return NinvalidArg;
    *out_got = 0;
    if (len == 0)
        return NSTATUS_OK;

    flags = irq_save();

    while (g_ready_count == 0 && g_eof_pending == 0) {
        Nstatus status = waitq_sleep_locked(&g_readers, 0);

        if (NSTATUS_IS_ERR(status)) {
            irq_restore(flags);
            return status;
        }
    }

    if (g_ready_count > 0) {
        /* 최대 한 줄: '\n' 을 복사하면 멈춘다 */
        while (n < len && g_ready_count > 0) {
            char c = g_ready[g_ready_head];

            buf[n++] = c;
            g_ready_head = (g_ready_head + 1U) % TTY_READY_MAX;
            g_ready_count--;
            if (c == '\n' && !g_raw)
                break;
        }
    } else {
        g_eof_pending--;                   /* EOF: 0 바이트 */
    }

    irq_restore(flags);
    *out_got = n;
    return NSTATUS_OK;
}

static void serial_irq_handler(struct trap_frame *frame)
{
    u32 guard = 0;

    (void)frame;
    if (!g_serial_rx)
        return;                            /* COM1 은 지금 사용자 프로세스 소유 */
    while (serial_rx_ready() && guard++ < 64U)
        tty_input_char(serial_getc_nowait());
}

void tty_init(void)
{
    waitq_init(&g_readers);

    keyboard_init();

    if (serial_available()) {
        if (irq_register_handler((u8)SERIAL_IRQ, serial_irq_handler) == NSTATUS_OK) {
            serial_enable_rx_interrupt();
            pic_clear_mask((u8)SERIAL_IRQ);
        }
    }
}
