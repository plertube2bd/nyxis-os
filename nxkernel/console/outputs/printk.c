/*
 * printk.c - 커널 로그 출력 (서식 처리만. 글자는 console/outputs/conout.c 장치로 보낸다)
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
#include "console/outputs/conout.h"
#include <stdarg.h>

#include "console/outputs/printk.h"
#include "console/outputs/ansi.h"
#include "lowlevel.h"

/*
 * 출력 경로 두 가지:
 *   kput           서식 문자열(커널 코드의 리터럴)과 숫자: 커널용 터미널 해석기를 거친다 -> ANSI 색 사용 가능
 *   kput_untrusted %s / %c 로 들어온 내용: 출력 가능한 ASCII 와 \n \r \t 만, 나머지는 '?'.
 *                  외부에서 온 문자열(파일 이름, 사용자 이름 ...)이 이스케이프 시퀀스를 주입하지 못한다.
 */
static void kput(char c)
{
    ansi_kernel_feed((u8)c);
}

static void kput_untrusted(char c)
{
    u8 uc = (u8)c;

    if (uc == '\n' || uc == '\r' || uc == '\t' || (uc >= 0x20 && uc <= 0x7E))
        conout_putc(c);
    else
        conout_putc('?');
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
        kput(' ');
        width--;
    }

    while (*s)
        kput_untrusted(*s++);
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
        kput('-');

    while (width > total) {
        kput(pad_zero ? '0' : ' ');
        width--;
    }

    if (neg && !pad_zero)
        kput('-');

    while (len > 0)
        kput(tmp[--len]);
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
            kput(*format++);
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
            kput('%');
            break;
        }
        format++;

        switch (spec) {
        case '%':
            kput('%');
            break;

        case 'c':
            kput_untrusted((char)va_arg(args, int));
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
            kput('0');
            kput('x');
            print_unsigned((u64)(usize)va_arg(args, void *), 16, false, 16, true);
            break;

        case 'r':
            kput('0');
            kput('x');
            print_unsigned((u64)(u32)va_arg(args, Nstatus), 16, false, 8, true);
            break;

        default:
            /* 알 수 없는 서식은 그대로 출력 (인자는 소비하지 않음) */
            kput('%');
            kput(spec);
            break;
        }
    }

    return NSTATUS_OK;
}

void printk_write(const char *buf, usize len)
{
    usize i;

    if (!buf)
        return;

    for (i = 0; i < len; i++) {
        u8 c = (u8)buf[i];

        if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c <= 0x7E))
            conout_putc((char)c);
        else
            conout_putc('?');
    }
}

/* 한 메시지를 통째로: 인터럽트(키보드 에코 등)가 중간에 끼어 색/커서 상태가 섞이지 않도록 막는다 */
static Nstatus printk_locked(const char *format, va_list args)
{
    u64 flags = irq_save();
    Nstatus status;

    conout_batch_begin();
    ansi_kernel_begin();
    status = printk_vformat(format, args);
    ansi_kernel_end();
    conout_batch_end();
    irq_restore(flags);
    return status;
}

Nstatus printk(const char *format, ...)
{
    va_list args;
    Nstatus status;

    va_start(args, format);
    status = printk_locked(format, args);
    va_end(args);

    return status;
}

/* 태그 붙은 로그: "[  OK  ] ..." (초록), "[ WARN ] ..." (노랑), "[FAILED] ..." (빨강) */
static Nstatus printk_tagged(const char *tag, const char *format, va_list args)
{
    u64 flags = irq_save();
    Nstatus status;
    const char *t;

    conout_batch_begin();
    ansi_kernel_begin();
    for (t = tag; *t; t++)
        kput(*t);
    status = printk_vformat(format, args);
    ansi_kernel_end();
    conout_batch_end();
    irq_restore(flags);
    return status;
}

Nstatus printk_ok(const char *format, ...)
{
    va_list args;
    Nstatus status;

    va_start(args, format);
    status = printk_tagged(KC_RESET "[  " KC_GREEN "OK" KC_RESET "  ] ", format, args);
    va_end(args);
    return status;
}

Nstatus printk_warn(const char *format, ...)
{
    va_list args;
    Nstatus status;

    va_start(args, format);
    status = printk_tagged(KC_RESET "[ " KC_YELLOW "WARN" KC_RESET " ] ", format, args);
    va_end(args);
    return status;
}

Nstatus printk_err(const char *format, ...)
{
    va_list args;
    Nstatus status;

    va_start(args, format);
    status = printk_tagged(KC_RESET "[" KC_RED "FAILED" KC_RESET "] ", format, args);
    va_end(args);
    return status;
}
