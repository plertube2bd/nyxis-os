/*
 * printk.c - 커널 로그 출력
 *
 * [수정 이력 요약]
 *  - 512바이트 스택 버퍼에 먼저 포맷하던 구조를 "글자 단위 스트리밍" 으로 변경.
 *    (기존: 긴 메시지는 아무것도 출력하지 않고 Noverflow 로 사라졌음)
 *  - %c, %p, %lx, %llx, %ld, 폭/0채움 지원 추가. 예외 덤프에서 64비트 주소를
 *    찍으려면 필요하다. (기존 %x 는 32비트만 출력)
 *  - 시리얼(COM1) 동시 출력. 프레임버퍼가 없어도 로그가 남는다.
 *  - 화면 하단 도달 시 위로 스크롤한다. (기존: 맨 위로 돌아가 이전 글 위에 덮어씀)
 *  - 모든 그리기는 draw_char_bounded 로 경계 검사를 한다.
 *  - C89 호환 (블록 시작에서만 선언).
 *
 * 보안 참고: printk(str) 처럼 신뢰할 수 없는 문자열을 "서식 문자열" 로 넘기면
 *   %s/%x 로 커널 스택/메모리를 읽어낼 수 있다. 외부 입력은 반드시
 *   printk("%s", str) 로 출력할 것.
 */

#include "nyxis.h"
#include "memory.h"
#include "console/outputs/outputs_base.h"
#include "console/outputs/serial.h"
#include <stdarg.h>

#include "console/outputs/printk.h"

/* 프레임버퍼 콘솔 상태 */
static u32 *g_fb = nNULL;
static u32  g_stride = 0;          /* 한 줄의 픽셀 수 (pixels_per_scan_line) */
static u32  g_screen_width = 0;
static u32  g_screen_height = 0;
static u32  g_cursor_x = 0;        /* 픽셀 단위 */
static u32  g_cursor_y = 0;

#define CONSOLE_FG  0xFFFFFFU
#define CONSOLE_BG  0x000000U

/* 비정상적으로 큰 해상도를 거부 (산술 오버플로/오설정 방지) */
#define CONSOLE_MAX_DIM 16384U

Nstatus printk_init(u32 *framebuffer, u32 pixels_per_scanline, u32 screen_width, u32 screen_height)
{
    usize row;

    if (!framebuffer || !pixels_per_scanline || !screen_width || !screen_height)
        return NinvalidArg;

    if (screen_width > CONSOLE_MAX_DIM || screen_height > CONSOLE_MAX_DIM)
        return NinvalidArg;

    if (pixels_per_scanline < screen_width || pixels_per_scanline > CONSOLE_MAX_DIM * 2U)
        return NinvalidArg;

    if (screen_width < FONT_W || screen_height < FONT_H)
        return NinvalidArg;

    g_fb = framebuffer;
    g_stride = pixels_per_scanline;
    g_screen_width = screen_width;
    g_screen_height = screen_height;
    g_cursor_x = 0;
    g_cursor_y = 0;

    /* 화면 전체를 배경색으로 지운다 (부트로더가 남긴 텍스트/테스트 패턴 제거) */
    for (row = 0; row < screen_height; row++) {
        u32 col;
        u32 *line = &g_fb[row * pixels_per_scanline];

        for (col = 0; col < screen_width; col++)
            line[col] = CONSOLE_BG;
    }

    return NSTATUS_OK;
}

/* 한 줄(FONT_H 픽셀) 위로 스크롤하고 맨 아래 줄을 지운다 */
static void console_scroll(void)
{
    usize row;
    usize rows_to_move = (usize)g_screen_height - FONT_H;

    memmove(g_fb,
            &g_fb[(usize)FONT_H * g_stride],
            rows_to_move * g_stride * sizeof(u32));

    for (row = rows_to_move; row < g_screen_height; row++) {
        u32 col;
        u32 *line = &g_fb[row * g_stride];

        for (col = 0; col < g_screen_width; col++)
            line[col] = CONSOLE_BG;
    }
}

static void console_newline(void)
{
    g_cursor_x = 0;
    g_cursor_y += FONT_H;

    /* 다음 글자 줄이 화면에 온전히 들어오지 않으면 스크롤 */
    while (g_cursor_y + FONT_H > g_screen_height) {
        console_scroll();
        g_cursor_y -= FONT_H;
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
        u32 tab = FONT_W * 4U;
        g_cursor_x = ((g_cursor_x / tab) + 1U) * tab;
        if (g_cursor_x + FONT_W > g_screen_width)
            console_newline();
        return;
    }

    if (g_cursor_x + FONT_W > g_screen_width)
        console_newline();

    (void)draw_char_bounded(g_fb, g_stride, g_screen_width, g_screen_height,
                            g_cursor_x, g_cursor_y, (utf8)c,
                            CONSOLE_FG, CONSOLE_BG);
    g_cursor_x += FONT_W;
}

/* 모든 출력 싱크로 한 글자 보내기 */
static void printk_putc(char c)
{
    if (c == '\n')
        serial_putc('\r');   /* 터미널에서 줄 맨 앞으로 */
    serial_putc(c);
    fb_putc(c);
}

/* 문자열 출력. width 가 문자열 길이보다 크면 왼쪽을 공백으로 채운다. */
static void print_string(const char *s, u32 width)
{
    usize len = 0;

    if (!s)
        s = "(null)";

    while (s[len])
        len++;

    while (width > len) {
        printk_putc(' ');
        width--;
    }

    while (*s)
        printk_putc(*s++);
}

/*
 * 정수를 base 진법으로 출력한다.
 * width: 최소 폭 (부호 포함). 부족하면 pad_zero 에 따라 '0' 또는 ' ' 로 채운다.
 * neg  : 음수 부호('-')를 붙일지 여부. 공백 채움이면 부호가 숫자 바로 앞에,
 *        0 채움이면 부호가 맨 앞에 온다. (printf 와 동일: "%5d" -> "  -42", "%05d" -> "-0042")
 */
static void print_number(u64 value, u32 base, bool upper, u32 width, bool pad_zero, bool neg)
{
    char tmp[24];   /* 10진 64비트는 최대 20자리 */
    u32 len = 0;
    u32 total;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (value == 0) {
        tmp[len++] = '0';
    } else {
        while (value > 0 && len < sizeof(tmp)) {
            tmp[len++] = digits[value % base];
            value /= base;
        }
    }

    total = len + (neg ? 1U : 0U);

    if (neg && pad_zero)
        printk_putc('-');

    while (width > total) {
        printk_putc(pad_zero ? '0' : ' ');
        width--;
    }

    if (neg && !pad_zero)
        printk_putc('-');

    while (len > 0)
        printk_putc(tmp[--len]);
}

static void print_unsigned(u64 value, u32 base, bool upper, u32 width, bool pad_zero)
{
    print_number(value, base, upper, width, pad_zero, false);
}

static void print_signed(i64 value, u32 width, bool pad_zero)
{
    if (value < 0) {
        /* INT64_MIN 도 안전하도록 부호 없는 산술로 절댓값 계산 */
        print_number((u64)0 - (u64)value, 10, false, width, pad_zero, true);
    } else {
        print_number((u64)value, 10, false, width, pad_zero, false);
    }
}

static Nstatus printk_vformat(const char *format, va_list args)
{
    if (!format)
        return NinvalidArg;

    while (*format) {
        bool pad_zero = false;
        u32  width = 0;
        u32  longness = 0;   /* 0: int, 1: long, 2: long long(=long, LP64) */
        char spec;

        if (*format != '%') {
            printk_putc(*format++);
            continue;
        }

        format++;   /* '%' 건너뜀 */

        /* 플래그: 0 채움 */
        if (*format == '0') {
            pad_zero = true;
            format++;
        }

        /* 폭 (상한 64: 비정상적으로 큰 폭으로 오래 걸리는 것 방지) */
        while (*format >= '0' && *format <= '9') {
            width = width * 10U + (u32)(*format - '0');
            if (width > 64U)
                width = 64U;
            format++;
        }

        /* 길이 수정자 */
        while (*format == 'l' || *format == 'z') {
            longness++;
            format++;
        }

        spec = *format;
        if (spec == '\0') {
            printk_putc('%');
            break;
        }
        format++;

        switch (spec) {
        case '%':
            printk_putc('%');
            break;

        case 'c':
            printk_putc((char)va_arg(args, int));
            break;

        case 's':
            print_string(va_arg(args, const char *), width);
            break;

        case 'd':
        case 'i':
            if (longness)
                print_signed((i64)va_arg(args, long), width, pad_zero);
            else
                print_signed((i64)va_arg(args, int), width, pad_zero);
            break;

        case 'u':
            if (longness)
                print_unsigned((u64)va_arg(args, unsigned long), 10, false, width, pad_zero);
            else
                print_unsigned((u64)va_arg(args, unsigned int), 10, false, width, pad_zero);
            break;

        case 'x':
        case 'X':
            if (longness)
                print_unsigned((u64)va_arg(args, unsigned long), 16, spec == 'X', width, pad_zero);
            else
                print_unsigned((u64)va_arg(args, unsigned int), 16, spec == 'X', width, pad_zero);
            break;

        case 'p':
            printk_putc('0');
            printk_putc('x');
            print_unsigned((u64)(usize)va_arg(args, void *), 16, false, 16, true);
            break;

        case 'r':
            printk_putc('0');
            printk_putc('x');
            print_unsigned((u64)(u32)va_arg(args, Nstatus), 16, false, 8, true);
            break;

        default:
            /* 알 수 없는 서식은 그대로 출력 (인자는 소비하지 않음) */
            printk_putc('%');
            printk_putc(spec);
            break;
        }
    }

    return NSTATUS_OK;
}

Nstatus printk(const char *format, ...)
{
    va_list args;
    Nstatus status;

    va_start(args, format);
    status = printk_vformat(format, args);
    va_end(args);

    return status;
}
