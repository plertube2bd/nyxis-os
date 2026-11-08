/*
 * scanf.c - C89 7.9.6 서식 입력 (fscanf, scanf, sscanf + C99 의 v* 변형)
 *
 * 지원: 공백 지시자, 일반 문자, %[*][너비][h|l|L] 와 변환 d i o u x X p c s [ n % e E f g G.
 * 부동소수점 항목은 [부호] 숫자[.숫자] [e[부호]숫자] 를 모아 strtod 로 바꾼다 (정확한 반올림).
 * 대상은 길이 없음 = float *, l = double *, L = long double *.
 *
 * 입력원은 get/unget 두 함수로 추상화한다 (문자열 / FILE). 앞을 내다보는 데 필요한 되돌리기는
 * 언제나 한 글자뿐이라 ungetc 하나로 충분하다 (C89 가 보장하는 것도 한 글자).
 *
 * 반환값: 대입한 항목 수. 첫 변환 전에 입력이 끝나면(입력 실패) EOF.
 * 수 변환에서 값이 범위를 넘는 것은 C89 에서 정의되지 않은 동작이다: 여기서는 strtol/strtoul 처럼 포화.
 */
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

struct src {
    int (*get)(void *ctx);
    void (*unget)(int c, void *ctx);
    void *ctx;
    long nread;                 /* 지금까지 소비한 글자 수 (%n) */
};

static int sget(struct src *in)
{
    int c = in->get(in->ctx);

    if (c != EOF)
        in->nread++;
    return c;
}

static void sunget(struct src *in, int c)
{
    if (c != EOF) {
        in->unget(c, in->ctx);
        in->nread--;
    }
}

/* 공백을 건너뛴다. 반환: 다음 글자 (소비하지 않음), 입력 끝이면 EOF */
static int skip_space(struct src *in)
{
    int c;

    do {
        c = sget(in);
    } while (c != EOF && isspace(c));
    sunget(in, c);
    return c;
}

static int digit_in_base(int c, int base)
{
    int v;

    if (isdigit(c))
        v = c - '0';
    else if (c >= 'a' && c <= 'z')
        v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z')
        v = c - 'A' + 10;
    else
        return 0;
    return v < base;
}

/*
 * 정수 입력 항목을 buf 에 모은다 (부호, 0x 접두어, 숫자). width 0 = 제한 없음.
 * base 0 이면 접두어로 정한 진법을 *base 에 돌려준다.
 * 반환: 1 성공, 0 대응 실패, EOF 입력 실패 (아무것도 못 읽음)
 */
#define NUM_MAX 80
static int scan_int_item(struct src *in, char *buf, int width, int *base)
{
    int n = 0;
    int c;
    int ndigits = 0;
    int limit = (width > 0 && width < NUM_MAX - 1) ? width : NUM_MAX - 1;

    if (skip_space(in) == EOF)
        return EOF;
    c = sget(in);
    if ((c == '+' || c == '-') && n < limit) {
        buf[n++] = (char)c;
        c = sget(in);
    }
    if (c == '0' && n < limit) {
        buf[n++] = (char)c;
        ndigits = 1;
        c = sget(in);
        if ((*base == 0 || *base == 16) && (c == 'x' || c == 'X') && n < limit) {
            buf[n++] = (char)c;
            ndigits = 0;                    /* "0x" 뒤에는 16진 숫자가 와야 한다 */
            *base = 16;
            c = sget(in);
        } else if (*base == 0) {
            *base = 8;
        }
    }
    if (*base == 0)
        *base = 10;
    while (n < limit && c != EOF && digit_in_base(c, *base)) {
        buf[n++] = (char)c;
        ndigits++;
        c = sget(in);
    }
    sunget(in, c);
    buf[n] = '\0';
    if (ndigits == 0)
        return (n == 0 && c == EOF) ? EOF : 0;
    return 1;
}

/*
 * 부동소수점 입력 항목을 buf 에 모은다. 되돌리기가 한 글자뿐이므로, 지수 표시 'e' 뒤에 숫자가
 * 오지 않으면 그 항목은 완전한 수가 아니다(대응 실패) — C89 의 "가장 긴 접두어" 규칙 그대로.
 * 반환: 1 성공, 0 대응 실패, EOF 입력 실패
 */
static int scan_float_item(struct src *in, char *buf, int width)
{
    int n = 0;
    int c;
    int ndigits = 0;
    int limit = (width > 0 && width < NUM_MAX - 1) ? width : NUM_MAX - 1;

    if (skip_space(in) == EOF)
        return EOF;
    c = sget(in);
    if ((c == '+' || c == '-') && n < limit) {
        buf[n++] = (char)c;
        c = sget(in);
    }
    while (n < limit && c != EOF && isdigit(c)) {
        buf[n++] = (char)c;
        ndigits++;
        c = sget(in);
    }
    if (n < limit && c == '.') {
        buf[n++] = (char)c;
        c = sget(in);
        while (n < limit && c != EOF && isdigit(c)) {
            buf[n++] = (char)c;
            ndigits++;
            c = sget(in);
        }
    }
    if (ndigits == 0) {
        sunget(in, c);
        buf[n] = '\0';
        return (n == 0 && c == EOF) ? EOF : 0;
    }
    if (n < limit && (c == 'e' || c == 'E')) {
        int edigits = 0;

        buf[n++] = (char)c;
        c = sget(in);
        if (n < limit && (c == '+' || c == '-')) {
            buf[n++] = (char)c;
            c = sget(in);
        }
        while (n < limit && c != EOF && isdigit(c)) {
            buf[n++] = (char)c;
            edigits++;
            c = sget(in);
        }
        if (edigits == 0) {
            sunget(in, c);
            return 0;
        }
    }
    sunget(in, c);
    buf[n] = '\0';
    return 1;
}

/* %[...] 집합을 읽는다. fmt 는 '[' 다음을 가리킨다. 반환: ']' 다음, 형식이 잘못됐으면 NULL */
static const char *parse_scanset(const char *fmt, unsigned char set[256])
{
    int invert = 0;
    int i;

    memset(set, 0, 256);
    if (*fmt == '^') {
        invert = 1;
        fmt++;
    }
    if (*fmt == ']')                        /* 맨 앞의 ']' 는 집합의 원소 */
        set[(unsigned char)*fmt++] = 1;
    while (*fmt && *fmt != ']') {
        unsigned char lo = (unsigned char)*fmt;

        /* 범위 a-z (C89 에서는 구현 정의). 처음/끝의 '-' 는 글자 그대로 */
        if (fmt[1] == '-' && fmt[2] && fmt[2] != ']' && (unsigned char)fmt[2] >= lo) {
            unsigned char hi = (unsigned char)fmt[2];

            for (i = lo; i <= hi; i++)
                set[i] = 1;
            fmt += 3;
        } else {
            set[lo] = 1;
            fmt++;
        }
    }
    if (*fmt != ']')
        return NULL;
    if (invert) {
        for (i = 0; i < 256; i++)
            set[i] = !set[i];
    }
    return fmt + 1;
}

static int vscan(struct src *in, const char *fmt, va_list ap)
{
    int assigned = 0;
    int converted = 0;                      /* 입력을 소비한 변환 수 (억제된 것 포함) */
    int c;

    while (*fmt) {
        int suppress = 0;
        int width = 0;
        char len = 0;

        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*fmt))
                fmt++;
            (void)skip_space(in);
            continue;
        }
        if (*fmt != '%' || fmt[1] == '%') {
            int want = (unsigned char)*fmt;

            if (*fmt == '%') {              /* "%%": 공백을 건너뛰고 '%' 하나 */
                fmt++;
                (void)skip_space(in);
            }
            c = sget(in);
            if (c == EOF)
                goto input_failure;
            if (c != want) {
                sunget(in, c);
                return assigned;
            }
            fmt++;
            continue;
        }

        fmt++;
        if (*fmt == '*') {
            suppress = 1;
            fmt++;
        }
        while (isdigit((unsigned char)*fmt)) {
            if (width <= (INT_MAX - 9) / 10)
                width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (*fmt == 'h' || *fmt == 'l' || *fmt == 'L')
            len = *fmt++;

        switch (*fmt) {
        case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'p': {
            char buf[NUM_MAX];
            int base = (*fmt == 'd' || *fmt == 'u') ? 10 : *fmt == 'i' ? 0 : *fmt == 'o' ? 8 : 16;
            int r = scan_int_item(in, buf, width, &base);

            if (r == EOF)
                goto input_failure;
            if (r == 0)
                return assigned;
            converted++;
            if (suppress)
                break;
            if (*fmt == 'p') {
                *va_arg(ap, void **) = (void *)strtoul(buf, NULL, 16);
            } else if (*fmt == 'd' || *fmt == 'i') {
                long v = strtol(buf, NULL, base);

                if (len == 'h')
                    *va_arg(ap, short *) = (short)v;
                else if (len == 'l')
                    *va_arg(ap, long *) = v;
                else
                    *va_arg(ap, int *) = (int)v;
            } else {
                unsigned long v = strtoul(buf, NULL, base);

                if (len == 'h')
                    *va_arg(ap, unsigned short *) = (unsigned short)v;
                else if (len == 'l')
                    *va_arg(ap, unsigned long *) = v;
                else
                    *va_arg(ap, unsigned int *) = (unsigned int)v;
            }
            assigned++;
            break;
        }
        case 'c': {
            char *dst = suppress ? NULL : va_arg(ap, char *);
            int i;

            if (width == 0)
                width = 1;
            for (i = 0; i < width; i++) {   /* 공백을 건너뛰지 않는다 */
                c = sget(in);
                if (c == EOF) {
                    if (i == 0)
                        goto input_failure;
                    return assigned;        /* 모자라면 입력 실패: 대입하지 않은 것으로 */
                }
                if (dst)
                    dst[i] = (char)c;
            }
            converted++;
            if (dst)
                assigned++;
            break;
        }
        case 's':
        case '[': {
            unsigned char set[256];
            char *dst;
            int i = 0;

            if (*fmt == '[') {
                fmt = parse_scanset(fmt + 1, set);
                if (fmt == NULL)
                    return assigned;        /* 잘못된 서식 */
                fmt--;                      /* 아래의 fmt++ 가 ']' 다음으로 */
            } else {
                int k;

                for (k = 0; k < 256; k++)
                    set[k] = !isspace(k);
                set[0] = 1;                 /* NUL 바이트도 공백이 아니다 */
                if (skip_space(in) == EOF)
                    goto input_failure;
            }
            dst = suppress ? NULL : va_arg(ap, char *);
            for (;;) {
                if (width > 0 && i >= width)
                    break;
                c = sget(in);
                if (c == EOF)
                    break;
                if (!set[(unsigned char)c]) {
                    sunget(in, c);
                    break;
                }
                if (dst)
                    dst[i] = (char)c;
                i++;
            }
            if (i == 0) {
                if (c == EOF)
                    goto input_failure;
                return assigned;
            }
            converted++;
            if (dst) {
                dst[i] = '\0';
                assigned++;
            }
            break;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G': {
            char buf[NUM_MAX];
            int r = scan_float_item(in, buf, width);
            double v;

            if (r == EOF)
                goto input_failure;
            if (r == 0)
                return assigned;
            converted++;
            if (suppress)
                break;
            v = strtod(buf, NULL);
            if (len == 'l')
                *va_arg(ap, double *) = v;
            else if (len == 'L')
                *va_arg(ap, long double *) = v;
            else
                *va_arg(ap, float *) = (float)v;
            assigned++;
            break;
        }
        case 'n':                           /* 입력을 소비하지 않고, 대입 수에도 세지 않는다 */
            if (!suppress) {
                if (len == 'h')
                    *va_arg(ap, short *) = (short)in->nread;
                else if (len == 'l')
                    *va_arg(ap, long *) = in->nread;
                else
                    *va_arg(ap, int *) = (int)in->nread;
            }
            break;
        default:                            /* 모르는 변환 */
            return assigned;
        }
        fmt++;
    }
    return assigned;

input_failure:
    return (converted == 0 && assigned == 0) ? EOF : assigned;
}

/* ---------------- 입력원 ---------------- */
struct strsrc {
    const unsigned char *s;
};

static int str_get(void *ctx)
{
    struct strsrc *p = (struct strsrc *)ctx;

    if (*p->s == '\0')
        return EOF;
    return *p->s++;
}

static void str_unget(int c, void *ctx)
{
    (void)c;
    ((struct strsrc *)ctx)->s--;
}

static int file_get(void *ctx)
{
    return fgetc((FILE *)ctx);
}

static void file_unget(int c, void *ctx)
{
    (void)ungetc(c, (FILE *)ctx);
}

int vsscanf(const char *s, const char *format, va_list arg)
{
    struct strsrc ss;
    struct src in;

    ss.s = (const unsigned char *)s;
    in.get = str_get;
    in.unget = str_unget;
    in.ctx = &ss;
    in.nread = 0;
    return vscan(&in, format, arg);
}

int vfscanf(FILE *stream, const char *format, va_list arg)
{
    struct src in;

    in.get = file_get;
    in.unget = file_unget;
    in.ctx = stream;
    in.nread = 0;
    return vscan(&in, format, arg);
}

int vscanf(const char *format, va_list arg)
{
    return vfscanf(stdin, format, arg);
}

int sscanf(const char *s, const char *format, ...)
{
    va_list ap;
    int r;

    va_start(ap, format);
    r = vsscanf(s, format, ap);
    va_end(ap);
    return r;
}

int fscanf(FILE *stream, const char *format, ...)
{
    va_list ap;
    int r;

    va_start(ap, format);
    r = vfscanf(stream, format, ap);
    va_end(ap);
    return r;
}

int scanf(const char *format, ...)
{
    va_list ap;
    int r;

    va_start(ap, format);
    r = vfscanf(stdin, format, ap);
    va_end(ap);
    return r;
}
