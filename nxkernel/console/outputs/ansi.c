/*
 * ansi.c - 터미널 출력 해석기. 지원 범위와 보안 규칙은 ansi.h 참고.
 */

#include "nyxis.h"
#include "console/outputs/conout.h"
#include "console/outputs/ansi.h"

#define ESC 0x1B

enum { ST_GROUND, ST_ESC, ST_CSI, ST_STRING, ST_STRING_ESC };

/* 색 속성 (SGR) */
enum { COL_DEFAULT, COL_INDEX, COL_RGB };
typedef struct {
    u32 kind;
    u32 value;                  /* COL_INDEX: 0..255, COL_RGB: 0xRRGGBB */
} term_color;

/*
 * 해석기 인스턴스: 사용자 출력(tty)과 커널 로그(printk)가 따로 갖는다.
 * 그래서 커널 로그가 사용자 프로그램이 쓰다 만 시퀀스에 끼어들거나 그 색을 덮어쓰지 않는다
 * (printk 가 끝나면 ansi_kernel_end() 가 tty 쪽 색을 되살린다).
 */
typedef struct {
    u32  state;
    u32  params[ANSI_MAX_PARAMS];
    u32  cur;                   /* 지금 채우는 매개변수 번호 */
    bool any;                   /* CSI 뒤에 매개변수 바이트가 하나라도 있었나 */
    bool priv;                  /* CSI ? ... */
    bool bad;                   /* 지원하지 않는 형식: 끝 바이트까지 먹고 버린다 */
    u32  seqlen;
    term_color fg;
    term_color bg;
    bool bold;
    bool reverse;
    u32  saved_row;
    u32  saved_col;
    bool colors_touched;        /* 이 인스턴스가 conout 의 색을 바꿨나 (커널 쪽에서만 씀) */
} ansi_state;

static ansi_state g_tty_term;
static ansi_state g_kern_term;
static ansi_state *T = &g_tty_term;   /* 지금 해석 중인 인스턴스 */

#define g_state      (T->state)
#define g_params     (T->params)
#define g_cur        (T->cur)
#define g_any        (T->any)
#define g_private    (T->priv)
#define g_bad        (T->bad)
#define g_seqlen     (T->seqlen)
#define g_fg         (T->fg)
#define g_bg         (T->bg)
#define g_bold       (T->bold)
#define g_reverse    (T->reverse)
#define g_saved_row  (T->saved_row)
#define g_saved_col  (T->saved_col)

/* xterm 기본 16색 */
static const u32 g_palette16[16] = {
    0x000000U, 0xCD0000U, 0x00CD00U, 0xCDCD00U, 0x0000EEU, 0xCD00CDU, 0x00CDCDU, 0xE5E5E5U,
    0x7F7F7FU, 0xFF0000U, 0x00FF00U, 0xFFFF00U, 0x5C5CFFU, 0xFF00FFU, 0x00FFFFU, 0xFFFFFFU
};

/* 256색 팔레트: 0-15 기본, 16-231 6x6x6 색 정육면체, 232-255 회색 */
static u32 palette256(u32 idx)
{
    static const u8 level[6] = { 0, 95, 135, 175, 215, 255 };

    if (idx < 16U)
        return g_palette16[idx];
    if (idx < 232U) {
        u32 i = idx - 16U;

        return ((u32)level[i / 36U] << 16) | ((u32)level[(i / 6U) % 6U] << 8) | (u32)level[i % 6U];
    }
    {
        u32 g = 8U + (idx - 232U) * 10U;

        return (g << 16) | (g << 8) | g;
    }
}

static u32 resolve(const term_color *c, u32 def, bool bright)
{
    if (c->kind == COL_INDEX)
        return palette256((bright && c->value < 8U) ? c->value + 8U : c->value);
    if (c->kind == COL_RGB)
        return c->value;
    return def;
}

static void apply_colors(void)
{
    u32 fg;
    u32 bg;

    T->colors_touched = true;
    fg = resolve(&g_fg, CONOUT_DEFAULT_FG, g_bold);
    bg = resolve(&g_bg, CONOUT_DEFAULT_BG, false);

    if (g_reverse)
        conout_set_color(bg, fg);
    else
        conout_set_color(fg, bg);
}

static void sgr_reset(void)
{
    g_fg.kind = COL_DEFAULT;
    g_bg.kind = COL_DEFAULT;
    g_bold = false;
    g_reverse = false;
}

/* ===================== */
/* 시리얼로 다시 만들어 보내기 */
/* ===================== */

static void emit_csi(char final)
{
    char buf[ANSI_MAX_PARAMS * 5U + 8U];
    usize n = 0;
    u32 i;

    buf[n++] = (char)ESC;
    buf[n++] = '[';
    if (g_private)
        buf[n++] = '?';
    if (g_any) {
        for (i = 0; i <= g_cur; i++) {
            char digits[5];
            u32 v = g_params[i];   /* <= ANSI_PARAM_MAX (4자리) */
            u32 nd = 0;

            if (i > 0)
                buf[n++] = ';';
            do {
                digits[nd++] = (char)('0' + v % 10U);
                v /= 10U;
            } while (v && nd < sizeof(digits));
            while (nd > 0)
                buf[n++] = digits[--nd];
        }
    }
    buf[n++] = final;
    conout_serial_write(buf, n);
}

/* ===================== */
/* CSI 처리               */
/* ===================== */

static u32 param(u32 i, u32 def)
{
    if (!g_any || i > g_cur || g_params[i] == 0)
        return def;
    return g_params[i];
}

/* 상대 이동: 화면 가장자리에서 멈춘다 */
static void move_rel(i32 drow, i32 dcol, bool col0)
{
    u32 row;
    u32 col;
    i32 r;
    i32 c;

    conout_get_cursor(&row, &col);
    r = (i32)row + drow;
    c = col0 ? 0 : (i32)col + dcol;
    conout_set_cursor(r < 0 ? 0U : (u32)r, c < 0 ? 0U : (u32)c);
}

/* 확장 색 (38/48 ;5;n 또는 ;2;r;g;b). 소비한 추가 매개변수 수를 돌려준다. 형식이 틀리면 -1 */
static i32 sgr_ext(u32 i, term_color *out)
{
    u32 count = g_cur + 1U;

    if (i + 1U >= count)
        return -1;
    if (g_params[i + 1U] == 5U) {
        if (i + 2U >= count || g_params[i + 2U] > 255U)
            return -1;
        out->kind = COL_INDEX;
        out->value = g_params[i + 2U];
        return 2;
    }
    if (g_params[i + 1U] == 2U) {
        if (i + 4U >= count || g_params[i + 2U] > 255U || g_params[i + 3U] > 255U || g_params[i + 4U] > 255U)
            return -1;
        out->kind = COL_RGB;
        out->value = (g_params[i + 2U] << 16) | (g_params[i + 3U] << 8) | g_params[i + 4U];
        return 4;
    }
    return -1;
}

/* SGR 적용. 형식이 틀린 확장 색을 만나면 거기서 멈추고 false (시퀀스를 시리얼로 보내지 않는다) */
static bool do_sgr(void)
{
    u32 i;

    if (!g_any) {
        sgr_reset();
        return true;
    }

    for (i = 0; i <= g_cur; i++) {
        u32 v = g_params[i];

        if (v == 0U)
            sgr_reset();
        else if (v == 1U)
            g_bold = true;
        else if (v == 22U)
            g_bold = false;
        else if (v == 7U)
            g_reverse = true;
        else if (v == 27U)
            g_reverse = false;
        else if (v >= 30U && v <= 37U) {
            g_fg.kind = COL_INDEX; g_fg.value = v - 30U;
        } else if (v >= 90U && v <= 97U) {
            g_fg.kind = COL_INDEX; g_fg.value = v - 90U + 8U;
        } else if (v == 39U)
            g_fg.kind = COL_DEFAULT;
        else if (v >= 40U && v <= 47U) {
            g_bg.kind = COL_INDEX; g_bg.value = v - 40U;
        } else if (v >= 100U && v <= 107U) {
            g_bg.kind = COL_INDEX; g_bg.value = v - 100U + 8U;
        } else if (v == 49U)
            g_bg.kind = COL_DEFAULT;
        else if (v == 38U || v == 48U) {
            i32 used = sgr_ext(i, v == 38U ? &g_fg : &g_bg);

            if (used < 0) {
                apply_colors();
                return false;
            }
            i += (u32)used;
        }
        /* 그 밖의 속성은 무시 */
    }
    return true;
}

static void dispatch_csi(char final)
{
    bool forward = true;
    u32 row;
    u32 col;

    if (g_bad)
        return;

    if (g_private) {
        /* CSI ?25h / ?25l 만: 커서 보이기/숨기기 */
        if ((final == 'h' || final == 'l') && g_any && g_cur == 0 && g_params[0] == 25U) {
            conout_set_cursor_visible(final == 'h' ? true : false);
            emit_csi(final);
        }
        return;
    }

    switch (final) {
    case 'A': move_rel(-(i32)param(0, 1), 0, false); break;
    case 'B': move_rel((i32)param(0, 1), 0, false); break;
    case 'C': move_rel(0, (i32)param(0, 1), false); break;
    case 'D': move_rel(0, -(i32)param(0, 1), false); break;
    case 'E': move_rel((i32)param(0, 1), 0, true); break;
    case 'F': move_rel(-(i32)param(0, 1), 0, true); break;
    case 'G':
        conout_get_cursor(&row, &col);
        conout_set_cursor(row, param(0, 1) - 1U);
        break;
    case 'd':
        conout_get_cursor(&row, &col);
        conout_set_cursor(param(0, 1) - 1U, col);
        break;
    case 'H':
    case 'f':
        conout_set_cursor(param(0, 1) - 1U, param(1, 1) - 1U);
        break;
    case 'J':
        if (g_any && g_cur > 0)
            return;
        if (param(0, 0) > 3U)
            return;
        conout_erase_display(param(0, 0));
        break;
    case 'K':
        if (g_any && g_cur > 0)
            return;
        if (param(0, 0) > 2U)
            return;
        conout_erase_line(param(0, 0));
        break;
    case 'm':
        forward = do_sgr();
        apply_colors();
        break;
    case 's':
        conout_get_cursor(&g_saved_row, &g_saved_col);
        break;
    case 'u':
        conout_set_cursor(g_saved_row, g_saved_col);
        break;
    default:
        return;    /* 지원하지 않는 시퀀스: 버린다 (시리얼로도 보내지 않는다) */
    }

    if (forward)
        emit_csi(final);
}

/* ===================== */
/* 바이트 입력             */
/* ===================== */

void ansi_reset(void)
{
    T = &g_tty_term;
    g_state = ST_GROUND;
    sgr_reset();
    apply_colors();
}

static void csi_begin(void)
{
    u32 i;

    for (i = 0; i < ANSI_MAX_PARAMS; i++)
        g_params[i] = 0;
    g_cur = 0;
    g_any = false;
    g_private = false;
    g_bad = false;
    g_seqlen = 2;
    g_state = ST_CSI;
}

static void ground(u8 c)
{
    if (c == ESC) {
        g_state = ST_ESC;
    } else if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c <= 0x7E)) {
        conout_putc((char)c);
    } else if (c == '\b') {
        u32 row;
        u32 col;

        conout_get_cursor(&row, &col);
        if (col > 0)
            conout_set_cursor(row, col - 1U);
        conout_serial_write("\b", 1);
    } else if (c == 0x07) {
        /* BEL: 무시 */
    } else {
        conout_putc('?');
    }
}

static void esc(u8 c)
{
    g_state = ST_GROUND;

    switch (c) {
    case '[':
        csi_begin();
        break;
    case '7':
        conout_get_cursor(&g_saved_row, &g_saved_col);
        conout_serial_write("\0337", 2);
        break;
    case '8':
        conout_set_cursor(g_saved_row, g_saved_col);
        conout_serial_write("\0338", 2);
        break;
    case 'c':
        conout_set_cursor_visible(true);
        sgr_reset();
        apply_colors();
        conout_erase_display(2);
        conout_set_cursor(0, 0);
        conout_serial_write("\033[0m\033[H\033[2J", 11);
        break;
    case ']':      /* OSC */
    case 'P':      /* DCS */
    case 'X':      /* SOS */
    case '^':      /* PM */
    case '_':      /* APC */
        g_seqlen = 2;
        g_state = ST_STRING;   /* 문자열 시퀀스: 종료 문자까지 통째로 버린다 */
        break;
    case ESC:
        g_state = ST_ESC;
        break;
    default:
        break;     /* 지원하지 않는 ESC 시퀀스는 버린다 */
    }
}

static void csi(u8 c)
{
    if (++g_seqlen > ANSI_MAX_SEQ)
        g_bad = true;

    if (c >= '0' && c <= '9') {
        u32 *p = &g_params[g_cur];

        *p = *p * 10U + (u32)(c - '0');
        if (*p > ANSI_PARAM_MAX)
            *p = ANSI_PARAM_MAX;
        g_any = true;
    } else if (c == ';') {
        if (g_cur + 1U >= ANSI_MAX_PARAMS)
            g_bad = true;
        else
            g_cur++;
        g_any = true;
    } else if (c == '?' && g_seqlen == 3U) {
        g_private = true;
    } else if (c >= 0x20 && c <= 0x3F) {
        g_bad = true;                         /* 중간 바이트, ':' '<' '=' '>' 등: 지원 안 함 */
    } else if (c >= 0x40 && c <= 0x7E) {
        g_state = ST_GROUND;
        dispatch_csi((char)c);
    } else if (c == ESC) {
        g_state = ST_ESC;                     /* 시퀀스 중단, 새 시퀀스 시작 */
    } else {
        g_state = ST_GROUND;                  /* 제어 바이트: 시퀀스를 버리고 그 바이트는 평소대로 */
        ground(c);
    }
}

/*
 * OSC/DCS 등 문자열 시퀀스: BEL 또는 ST(ESC \) 까지 버린다. 종료 문자가 오지 않아도 화면이 영영 먹통이
 * 되지 않도록 ANSI_MAX_STRING 바이트를 넘으면 포기하고 평소 상태로 돌아간다.
 */
static void string_seq(u8 c)
{
    if (c == 0x07 || ++g_seqlen > ANSI_MAX_STRING) {
        g_state = ST_GROUND;
    } else if (c == ESC) {
        g_state = ST_STRING_ESC;
    }
}

static void string_esc(u8 c)
{
    if (c == '\\') {
        g_state = ST_GROUND;   /* ST */
    } else {
        g_state = ST_ESC;      /* 문자열이 끝나지 않은 채 새 시퀀스: xterm 처럼 문자열을 끝내고 처리 */
        esc(c);
    }
}

static void ansi_step(u8 c);

/* 현재 인스턴스의 색 속성을 시리얼로 다시 보낸다 (ESC[0;...m) */
static void emit_sgr_state(void)
{
    u32 saved_params[ANSI_MAX_PARAMS];
    u32 n = 0;
    u32 i;
    const term_color *cs[2];

    for (i = 0; i < ANSI_MAX_PARAMS; i++)
        saved_params[i] = g_params[i];

    g_params[n++] = 0;
    if (g_bold)
        g_params[n++] = 1;
    if (g_reverse)
        g_params[n++] = 7;
    cs[0] = &g_fg;
    cs[1] = &g_bg;
    for (i = 0; i < 2; i++) {
        const term_color *c = cs[i];

        if (c->kind == COL_INDEX) {
            g_params[n++] = i == 0 ? 38U : 48U;
            g_params[n++] = 5;
            g_params[n++] = c->value;
        } else if (c->kind == COL_RGB) {
            g_params[n++] = i == 0 ? 38U : 48U;
            g_params[n++] = 2;
            g_params[n++] = (c->value >> 16) & 0xFFU;
            g_params[n++] = (c->value >> 8) & 0xFFU;
            g_params[n++] = c->value & 0xFFU;
        }
    }
    /* n <= 1 + 1 + 1 + 5 + 5 = 13 < ANSI_MAX_PARAMS */
    g_cur = n - 1U;
    g_any = true;
    g_private = false;
    emit_csi('m');

    for (i = 0; i < ANSI_MAX_PARAMS; i++)
        g_params[i] = saved_params[i];
}

void ansi_kernel_begin(void)
{
    T = &g_kern_term;
    g_state = ST_GROUND;
    sgr_reset();
    T->colors_touched = false;
}

void ansi_kernel_feed(u8 c)
{
    T = &g_kern_term;
    ansi_step(c);
}

void ansi_kernel_end(void)
{
    bool touched = g_kern_term.colors_touched;

    T = &g_tty_term;
    if (touched) {
        /* 사용자 출력 쪽 색을 되살린다 (프레임버퍼 + 시리얼). 진행 중이던 tty 시퀀스 상태는 건드리지 않는다 */
        u32 cur = g_cur;
        bool any = g_any;
        bool priv = g_private;

        apply_colors();
        emit_sgr_state();
        g_cur = cur;
        g_any = any;
        g_private = priv;
    }
}

void ansi_feed(u8 c)
{
    T = &g_tty_term;
    ansi_step(c);
}

static void ansi_step(u8 c)
{
    switch (g_state) {
    case ST_STRING:
        string_seq(c);
        break;
    case ST_STRING_ESC:
        string_esc(c);
        break;
    case ST_ESC:
        esc(c);
        break;
    case ST_CSI:
        csi(c);
        break;
    default:
        ground(c);
        break;
    }
}
