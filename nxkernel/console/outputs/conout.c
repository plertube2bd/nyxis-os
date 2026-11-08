/*
 * conout.c - 콘솔 출력 장치 (프레임버퍼 글자 콘솔 + COM1 시리얼). 설계는 conout.h 참고.
 */

#include "nyxis.h"
#include "memory.h"
#include "console/outputs/outputs_base.h"
#include "console/outputs/serial.h"
#include "console/outputs/conout.h"
#include "lowlevel.h"

/* 프레임버퍼 콘솔 상태 */
static u32 *g_fb = nNULL;
static u32  g_stride = 0;          /* 한 줄의 픽셀 수 (pixels_per_scan_line) */
static u32  g_screen_width = 0;
static u32  g_screen_height = 0;
static u32  g_cursor_x = 0;        /* 픽셀 단위 (항상 셀 경계) */
static u32  g_cursor_y = 0;
static u32  g_cell_w = 16;         /* 글자 셀 크기: font_init() 결과에 따라 정해진다 */
static u32  g_cell_h = 16;
static bool g_red_low = false;     /* NTBLI_PIXEL_RGB: 메모리 첫 바이트가 R (u32 로는 0x00BBGGRR) */

/* 커서: 글자 칸 하나를 반전(XOR)해서 보인다. XOR 라 같은 칸을 한 번 더 뒤집으면 정확히 원래대로 돌아온다.
 * 프레임버퍼를 바꾸는 공개 함수는 모두 lock_begin() 에서 커서를 지우고 lock_end() 에서 다시 그린다. */
static bool g_cursor_visible = true;   /* CSI ?25 h/l */
static u32  g_batch = 0;               /* conout_batch_begin 중첩 수: 0 이 아니면 커서를 다시 그리지 않는다 */
static bool g_cursor_drawn = false;
static u32  g_cursor_drawn_x = 0;
static u32  g_cursor_drawn_y = 0;

/* 현재 색 (프레임버퍼 원래 형식으로 변환된 값) */
static u32  g_fg_px = 0xFFFFFFU;
static u32  g_bg_px = 0x000000U;

/* 비정상적으로 큰 해상도를 거부 (산술 오버플로/오설정 방지) */
#define CONSOLE_MAX_DIM 16384U

/* 0xRRGGBB -> 프레임버퍼 픽셀 */
static u32 rgb_to_px(u32 rgb)
{
    rgb &= 0xFFFFFFU;
    if (g_red_low)
        return ((rgb >> 16) & 0xFFU) | (rgb & 0xFF00U) | ((rgb & 0xFFU) << 16);
    return rgb;
}

/* 픽셀 영역 [x0, x1) x [y0, y1) 을 현재 배경색으로 칠한다 (호출자가 범위를 보장) */
static void fill_rect(u32 x0, u32 y0, u32 x1, u32 y1)
{
    u32 y;

    for (y = y0; y < y1; y++) {
        u32 *line = &g_fb[(usize)y * g_stride];
        u32 x;

        for (x = x0; x < x1; x++)
            line[x] = g_bg_px;
    }
}

static volatile bool g_out_fb = true;
static volatile bool g_out_serial = true;

static void xor_cell(u32 x, u32 y)
{
    u32 row;

    for (row = 0; row < g_cell_h; row++) {
        u32 *line = &g_fb[(usize)(y + row) * g_stride + x];
        u32 col;

        for (col = 0; col < g_cell_w; col++)
            line[col] ^= 0x00FFFFFFU;
    }
}

static void cursor_hide(void)
{
    if (g_cursor_drawn) {
        xor_cell(g_cursor_drawn_x, g_cursor_drawn_y);
        g_cursor_drawn = false;
    }
}

static void cursor_show(void)
{
    if (g_cursor_drawn || !g_cursor_visible || !g_out_fb || !g_fb)
        return;
    /* 줄 끝을 막 지난 자리(다음 글자에서 줄바꿈 예정)에는 그리지 않는다 */
    if (g_cursor_x + g_cell_w > g_screen_width || g_cursor_y + g_cell_h > g_screen_height)
        return;
    xor_cell(g_cursor_x, g_cursor_y);
    g_cursor_drawn_x = g_cursor_x;
    g_cursor_drawn_y = g_cursor_y;
    g_cursor_drawn = true;
}

/* 프레임버퍼 작업 구간: 인터럽트(키보드 에코)와 섞이지 않게 막고, 그 동안 커서를 지워 둔다 */
static u64 lock_begin(void)
{
    u64 flags = irq_save();

    cursor_hide();
    return flags;
}

static void lock_end(u64 flags)
{
    if (g_batch == 0)
        cursor_show();
    irq_restore(flags);
}

void conout_batch_begin(void)
{
    u64 flags = irq_save();

    g_batch++;
    irq_restore(flags);
}

void conout_batch_end(void)
{
    u64 flags = irq_save();

    if (g_batch > 0)
        g_batch--;
    if (g_batch == 0 && g_out_fb)
        cursor_show();
    irq_restore(flags);
}

Nstatus conout_init(u32 *framebuffer, u32 pixels_per_scanline, u32 screen_width, u32 screen_height,
                    u32 pixel_format)
{
    if (!framebuffer || !pixels_per_scanline || !screen_width || !screen_height)
        return NinvalidArg;

    if (screen_width > CONSOLE_MAX_DIM || screen_height > CONSOLE_MAX_DIM)
        return NinvalidArg;

    if (pixels_per_scanline < screen_width || pixels_per_scanline > CONSOLE_MAX_DIM * 2U)
        return NinvalidArg;

    if (pixel_format != NTBLI_PIXEL_RGB && pixel_format != NTBLI_PIXEL_BGR)
        return NinvalidArg;

    (void)font_init();   /* 실패하면 16x16 비트맵으로 되돌아간다 (kernel.c 가 font_is_ttf() 로 알린다) */
    g_cell_w = font_cell_width();
    g_cell_h = font_cell_height();

    if (screen_width < g_cell_w || screen_height < g_cell_h)
        return NinvalidArg;

    g_fb = framebuffer;
    g_stride = pixels_per_scanline;
    g_screen_width = screen_width;
    g_screen_height = screen_height;
    g_red_low = (pixel_format == NTBLI_PIXEL_RGB) ? true : false;
    g_cursor_x = 0;
    g_cursor_y = 0;
    conout_reset_color();

    /* 화면 전체를 배경색으로 지운다 (부트로더가 남긴 텍스트/테스트 패턴 제거) */
    g_cursor_drawn = false;
    fill_rect(0, 0, screen_width, screen_height);
    cursor_show();

    return NSTATUS_OK;
}

/* 한 줄(g_cell_h 픽셀) 위로 스크롤하고 맨 아래 줄을 지운다 */
static void console_scroll(void)
{
    usize rows_to_move = (usize)g_screen_height - g_cell_h;

    memmove(g_fb,
            &g_fb[(usize)g_cell_h * g_stride],
            rows_to_move * g_stride * sizeof(u32));

    fill_rect(0, (u32)rows_to_move, g_screen_width, g_screen_height);
}

static void console_newline(void)
{
    g_cursor_x = 0;
    g_cursor_y += g_cell_h;

    /* 다음 글자 줄이 화면에 온전히 들어오지 않으면 스크롤 */
    while (g_cursor_y + g_cell_h > g_screen_height) {
        console_scroll();
        g_cursor_y -= g_cell_h;
    }
}

/* 프레임버퍼에 한 글자 출력 */
static void fb_putc(char c)
{
    if (!g_fb)
        return;

    if (c == '\n') {
        console_newline();
        return;
    }

    if (c == '\r') {
        g_cursor_x = 0;
        return;
    }

    if (c == '\t') {
        /* 4글자 단위 탭 */
        u32 tab = g_cell_w * 4U;
        g_cursor_x = ((g_cursor_x / tab) + 1U) * tab;
        if (g_cursor_x + g_cell_w > g_screen_width)
            console_newline();
        return;
    }

    if (g_cursor_x + g_cell_w > g_screen_width)
        console_newline();

    (void)draw_char_bounded(g_fb, g_stride, g_screen_width, g_screen_height,
                            g_cursor_x, g_cursor_y, (utf8)c,
                            g_fg_px, g_bg_px);
    g_cursor_x += g_cell_w;
}

/* 출력 싱크 사용 여부(g_out_*): 사용자 프로세스가 장치(dev:/fb0, dev:/com1)를 배타적으로 가져가면 꺼진다 */

void conout_set_fb_enabled(bool on)
{
    u64 flags = irq_save();

    if (!on)
        cursor_hide();     /* 장치를 가져가는 프로세스의 화면에 커서 자국을 남기지 않는다 */
    g_out_fb = on;
    if (on)
        cursor_show();
    irq_restore(flags);
}

void conout_set_serial_enabled(bool on)
{
    g_out_serial = on;
}

void conout_force_all_outputs(void)
{
    u64 flags = irq_save();

    g_out_fb = true;
    g_out_serial = true;
    conout_reset_color();   /* 패닉 메시지가 사용자 프로그램이 남긴 색에 묻히지 않도록 */
    irq_restore(flags);
}

/* 모든 출력 싱크로 한 글자 보내기 */
void conout_putc(char c)
{
    u64 flags;

    if (g_out_serial) {
        if (c == '\n')
            serial_putc('\r');   /* 터미널에서 줄 맨 앞으로 */
        serial_putc(c);
    }
    if (g_out_fb) {
        flags = lock_begin();
        fb_putc(c);
        lock_end(flags);
    }
}

/* 커서를 한 칸 왼쪽으로 (줄 처음이면 윗줄 끝으로). 프레임버퍼 전용 */
static void fb_cursor_back(void)
{
    if (g_cursor_x >= g_cell_w) {
        g_cursor_x -= g_cell_w;
    } else if (g_cursor_y >= g_cell_h) {
        g_cursor_y -= g_cell_h;
        g_cursor_x = (g_screen_width / g_cell_w - 1U) * g_cell_w;
    }
}

void conout_backspace(void)
{
    u64 flags;

    if (g_out_serial) {
        serial_putc('\b');
        serial_putc(' ');
        serial_putc('\b');
    }

    if (g_out_fb && g_fb && g_cursor_x >= g_cell_w) {
        flags = lock_begin();
        g_cursor_x -= g_cell_w;
        (void)draw_char_bounded(g_fb, g_stride, g_screen_width, g_screen_height,
                                g_cursor_x, g_cursor_y, (utf8)' ',
                                g_fg_px, g_bg_px);
        lock_end(flags);
    }
}

void conout_cursor_back(u32 n)
{
    u64 flags;
    u32 i;

    if (g_out_serial) {
        for (i = 0; i < n; i++)
            serial_putc('\b');
    }
    if (g_out_fb && g_fb) {
        flags = lock_begin();
        for (i = 0; i < n; i++)
            fb_cursor_back();
        lock_end(flags);
    }
}

void conout_set_cursor_visible(bool on)
{
    u64 flags = irq_save();

    if (!on)
        cursor_hide();
    g_cursor_visible = on;
    if (on)
        cursor_show();
    irq_restore(flags);
}

/* ===================== */
/* 터미널 제어 (ansi.c)    */
/* ===================== */

void conout_serial_write(const char *s, usize len)
{
    usize i;

    if (!g_out_serial || !s)
        return;
    for (i = 0; i < len; i++)
        serial_putc(s[i]);
}

void conout_set_color(u32 fg_rgb, u32 bg_rgb)
{
    g_fg_px = rgb_to_px(fg_rgb);
    g_bg_px = rgb_to_px(bg_rgb);
}

void conout_reset_color(void)
{
    conout_set_color(CONOUT_DEFAULT_FG, CONOUT_DEFAULT_BG);
}

void conout_get_size(u32 *rows, u32 *cols)
{
    *rows = g_fb ? g_screen_height / g_cell_h : 0;
    *cols = g_fb ? g_screen_width / g_cell_w : 0;
}

void conout_get_cursor(u32 *row, u32 *col)
{
    *row = g_fb ? g_cursor_y / g_cell_h : 0;
    *col = g_fb ? g_cursor_x / g_cell_w : 0;
}

void conout_set_cursor(u32 row, u32 col)
{
    u32 rows;
    u32 cols;
    u64 flags;

    if (!g_out_fb || !g_fb)
        return;
    conout_get_size(&rows, &cols);
    if (row >= rows)
        row = rows - 1U;
    if (col >= cols)
        col = cols - 1U;
    flags = lock_begin();
    g_cursor_y = row * g_cell_h;
    g_cursor_x = col * g_cell_w;
    lock_end(flags);
}

/* 줄 지우기 (잠금은 호출자가) */
static void erase_line_unlocked(u32 mode)
{
    u32 x0;
    u32 x1;

    switch (mode) {
    case 0:  x0 = g_cursor_x; x1 = g_screen_width; break;                 /* 커서부터 줄 끝 */
    case 1:  x0 = 0;          x1 = g_cursor_x + g_cell_w; break;          /* 줄 처음부터 커서(포함) */
    default: x0 = 0;          x1 = g_screen_width; break;                 /* 줄 전체 */
    }
    if (x1 > g_screen_width)
        x1 = g_screen_width;
    if (x0 >= x1)
        return;
    fill_rect(x0, g_cursor_y, x1, g_cursor_y + g_cell_h);
}

void conout_erase_display(u32 mode)
{
    u64 flags;

    if (!g_out_fb || !g_fb)
        return;

    flags = lock_begin();
    switch (mode) {
    case 0:   /* 커서부터 끝까지 */
        erase_line_unlocked(0);
        fill_rect(0, g_cursor_y + g_cell_h, g_screen_width, g_screen_height);
        break;
    case 1:   /* 처음부터 커서까지 */
        fill_rect(0, 0, g_screen_width, g_cursor_y);
        erase_line_unlocked(1);
        break;
    default:  /* 2, 3: 전체 (스크롤백이 없으므로 3 도 같다). 커서는 움직이지 않는다 */
        fill_rect(0, 0, g_screen_width, g_screen_height);
        break;
    }
    lock_end(flags);
}

void conout_erase_line(u32 mode)
{
    u64 flags;

    if (!g_out_fb || !g_fb)
        return;
    flags = lock_begin();
    erase_line_unlocked(mode);
    lock_end(flags);
}
