/*
 * printf.c - C89 7.9.6 서식 출력 엔진과 printf 계열
 *
 * 지원: 플래그 - + 공백 # 0, 너비/정밀도(* 포함), 길이 h l L, 변환 d i o u x X c s p n % e E f g G.
 *
 * 부동소수점은 fpconv.c 의 정확한 십진 전개 위에서 반올림한다(가장 가까운 값, 동점은 짝수) —
 * 그래서 %.20f 처럼 긴 정밀도도 근사 없이 정확한 숫자를 낸다. 무한대/NaN 은 inf/nan (대문자 변환은 INF/NAN).
 * long double(%Le 등)은 double 로 바꿔 출력한다 (정밀도가 double 만큼으로 줄어든다).
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include "nl_internal.h"

#define F_MINUS  0x01
#define F_PLUS   0x02
#define F_SPACE  0x04
#define F_HASH   0x08
#define F_ZERO   0x10

struct out {
    __nl_sink_fn sink;
    void *ctx;
    unsigned long count;
    int failed;
};

static void emit(struct out *o, const char *s, size_t n)
{
    if (n == 0 || o->failed)
        return;
    if (o->sink(o->ctx, s, n) < 0)
        o->failed = 1;
    else
        o->count += n;
}

static void emit_rep(struct out *o, char c, long n)
{
    char buf[32];
    size_t chunk;

    if (n <= 0)
        return;
    memset(buf, c, sizeof(buf));
    while (n > 0) {
        chunk = (unsigned long)n < sizeof(buf) ? (size_t)n : sizeof(buf);
        emit(o, buf, chunk);
        n -= (long)chunk;
    }
}

/* 정수 하나: prefix(부호/0x), 정밀도 0 채움, 너비 채움 */
static void emit_int(struct out *o, unsigned long v, int neg, int base, int upper,
                     int flags, int width, int prec, int is_signed, int is_ptr)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[24];
    int nd = 0;
    char prefix[3];
    int np = 0;
    long zeros;
    long total;

    while (v) {
        buf[sizeof(buf) - 1 - nd++] = digits[v % (unsigned long)base];
        v /= (unsigned long)base;
    }
    /* 정밀도 0 과 값 0 이면 숫자를 하나도 쓰지 않는다 (C89 7.9.6.1) */
    if (prec < 0)
        prec = 1;

    if (is_signed) {
        if (neg)
            prefix[np++] = '-';
        else if (flags & F_PLUS)
            prefix[np++] = '+';
        else if (flags & F_SPACE)
            prefix[np++] = ' ';
    }
    if (base == 16 && ((flags & F_HASH) || is_ptr) && (nd > 0 || is_ptr)) {
        prefix[np++] = '0';
        prefix[np++] = upper ? 'X' : 'x';
    }
    zeros = prec > nd ? prec - nd : 0;
    /* '#' 와 8진: 첫 숫자가 0 이 되도록 정밀도를 늘린다 */
    if (base == 8 && (flags & F_HASH) && zeros == 0)
        zeros = 1;

    total = np + zeros + nd;
    if (!(flags & F_MINUS) && !(flags & F_ZERO))
        emit_rep(o, ' ', width - total);
    emit(o, prefix, (size_t)np);
    if (!(flags & F_MINUS) && (flags & F_ZERO))
        emit_rep(o, '0', width - total);
    emit_rep(o, '0', zeros);
    emit(o, buf + sizeof(buf) - nd, (size_t)nd);
    if (flags & F_MINUS)
        emit_rep(o, ' ', width - total);
}

static void emit_str(struct out *o, const char *s, size_t len, int flags, int width)
{
    if (!(flags & F_MINUS))
        emit_rep(o, ' ', width - (long)len);
    emit(o, s, len);
    if (flags & F_MINUS)
        emit_rep(o, ' ', width - (long)len);
}

/* ---------------- 부동소수점 ---------------- */

/*
 * 숫자열 D[0..*nd) (값 = 0.D * 10^*dexp) 를 앞에서 keep 자리로 반올림한다 (가장 가까운 값, 동점은 짝수).
 * 올림이 맨 앞까지 번지면 "1" 이 되고 *dexp 가 1 늘어난다. 끝의 0 은 떼어낸다.
 */
static void round_digits(char *D, int *nd, int *dexp, int keep)
{
    int up = 0;
    int i;

    if (keep >= *nd)
        return;
    if (keep < 0) {                         /* 반올림 자리보다 값이 훨씬 작다: 0 */
        *nd = 0;
        return;
    }
    if (D[keep] > '5') {
        up = 1;
    } else if (D[keep] == '5') {
        for (i = keep + 1; i < *nd && D[i] == '0'; i++)
            ;
        if (i < *nd)
            up = 1;                         /* 5 뒤에 0 이 아닌 숫자: 중간보다 크다 */
        else
            up = keep > 0 && ((D[keep - 1] - '0') & 1);   /* 정확히 중간: 짝수 쪽으로 */
    }
    *nd = keep;
    if (up) {
        for (i = keep - 1; i >= 0; i--) {
            if (D[i] != '9') {
                D[i]++;
                break;
            }
            D[i] = '0';
        }
        if (i < 0) {                        /* 999 -> 1000 */
            D[0] = '1';
            *nd = 1;
            (*dexp)++;
        }
    }
    while (*nd > 0 && D[*nd - 1] == '0')
        (*nd)--;
    if (*nd == 0)
        *dexp = 0;
}

/* 여러 글자를 작은 버퍼에 모아서 내보낸다 */
struct chunk {
    struct out *o;
    char buf[64];
    size_t n;
};

static void ch_put(struct chunk *c, char ch)
{
    c->buf[c->n++] = ch;
    if (c->n == sizeof(c->buf)) {
        emit(c->o, c->buf, c->n);
        c->n = 0;
    }
}

static void ch_flush(struct chunk *c)
{
    emit(c->o, c->buf, c->n);
    c->n = 0;
}

static char digit_at(const char *D, int nd, int i)
{
    return (i >= 0 && i < nd) ? D[i] : '0';
}

static void emit_float(struct out *o, double v, int conv, int flags, int width, int prec)
{
    static char D[NL_DTOA_MAX];
    union { double d; unsigned long u; } bits;
    int neg;
    int nd;
    int dexp;
    int upper = (conv == 'E' || conv == 'G');
    int style;                              /* 'e' 또는 'f' */
    char sign = 0;
    long body;                              /* 부호를 뺀 출력 길이 */
    int x = 0;                              /* %e 의 지수 */
    char expbuf[8];
    int expn = 0;
    struct chunk c;
    int i;

    bits.d = v;
    neg = (int)(bits.u >> 63);
    if (neg)
        sign = '-';
    else if (flags & F_PLUS)
        sign = '+';
    else if (flags & F_SPACE)
        sign = ' ';

    if (((bits.u >> 52) & 0x7FF) == 0x7FF) {           /* 무한대 / NaN: 0 채움 없음 */
        const char *t = (bits.u & ((1UL << 52) - 1)) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        char tmp[4];
        int k = 0;

        if (sign)
            tmp[k++] = sign;
        if (!(flags & F_MINUS))
            emit_rep(o, ' ', width - (k + 3));
        emit(o, tmp, (size_t)k);
        emit(o, t, 3);
        if (flags & F_MINUS)
            emit_rep(o, ' ', width - (k + 3));
        return;
    }

    if (prec < 0)
        prec = 6;
    nd = __nl_dtoa(v, D, &dexp);

    if (conv == 'f') {
        style = 'f';
        round_digits(D, &nd, &dexp, dexp + prec);
    } else if (conv == 'e' || conv == 'E') {
        style = 'e';
        round_digits(D, &nd, &dexp, prec + 1);
    } else {                                /* %g: 유효 숫자 P 개로 반올림한 뒤 지수로 형식을 고른다 */
        int P = prec == 0 ? 1 : prec;

        round_digits(D, &nd, &dexp, P);
        x = nd == 0 ? 0 : dexp - 1;
        if (x < P && x >= -4) {
            style = 'f';
            prec = P - 1 - x;
        } else {
            style = 'e';
            prec = P - 1;
        }
        if (!(flags & F_HASH)) {            /* 숫자열 끝의 0 은 이미 없다: 보여 줄 소수 자릿수를 줄인다 */
            int need = style == 'f' ? nd - dexp : nd - 1;

            if (need < 0)
                need = 0;
            if (need < prec)
                prec = need;
        }
    }

    /* 길이 계산 */
    if (style == 'f') {
        body = (dexp > 0 ? dexp : 1) + prec + ((prec > 0 || (flags & F_HASH)) ? 1 : 0);
    } else {
        long e10 = nd == 0 ? 0 : (long)dexp - 1;
        unsigned long ae = (unsigned long)(e10 < 0 ? -e10 : e10);
        char rev[8];
        int k = 0;

        x = (int)e10;
        do {
            rev[k++] = (char)('0' + ae % 10);
            ae /= 10;
        } while (ae);
        if (k < 2)
            rev[k++] = '0';
        expbuf[expn++] = upper ? 'E' : 'e';
        expbuf[expn++] = x < 0 ? '-' : '+';
        while (k)
            expbuf[expn++] = rev[--k];
        body = 1 + prec + ((prec > 0 || (flags & F_HASH)) ? 1 : 0) + expn;
    }
    body += sign ? 1 : 0;

    if (!(flags & F_MINUS) && !(flags & F_ZERO))
        emit_rep(o, ' ', width - body);
    if (sign)
        emit(o, &sign, 1);
    if (!(flags & F_MINUS) && (flags & F_ZERO))
        emit_rep(o, '0', width - body);

    c.o = o;
    c.n = 0;
    if (style == 'f') {
        if (dexp > 0) {
            for (i = 0; i < dexp; i++)
                ch_put(&c, digit_at(D, nd, i));
        } else {
            ch_put(&c, '0');
        }
        if (prec > 0 || (flags & F_HASH))
            ch_put(&c, '.');
        for (i = 0; i < prec; i++)
            ch_put(&c, digit_at(D, nd, dexp + i));
    } else {
        ch_put(&c, digit_at(D, nd, 0));
        if (prec > 0 || (flags & F_HASH))
            ch_put(&c, '.');
        for (i = 0; i < prec; i++)
            ch_put(&c, digit_at(D, nd, 1 + i));
        for (i = 0; i < expn; i++)
            ch_put(&c, expbuf[i]);
    }
    ch_flush(&c);

    if (flags & F_MINUS)
        emit_rep(o, ' ', width - body);
}

int __nl_vformat(__nl_sink_fn sink, void *ctx, const char *fmt, va_list ap)
{
    struct out o;

    o.sink = sink;
    o.ctx = ctx;
    o.count = 0;
    o.failed = 0;

    while (*fmt) {
        const char *spec;
        int flags = 0;
        int width = 0;
        int prec = -1;
        char len = 0;

        if (*fmt != '%') {
            const char *p = fmt;

            while (*p && *p != '%')
                p++;
            emit(&o, fmt, (size_t)(p - fmt));
            fmt = p;
            continue;
        }
        spec = fmt++;

        for (;; fmt++) {
            if (*fmt == '-') flags |= F_MINUS;
            else if (*fmt == '+') flags |= F_PLUS;
            else if (*fmt == ' ') flags |= F_SPACE;
            else if (*fmt == '#') flags |= F_HASH;
            else if (*fmt == '0') flags |= F_ZERO;
            else break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {            /* 음수 너비 = '-' 플래그 */
                flags |= F_MINUS;
                width = width == INT_MIN ? INT_MAX : -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                if (width <= (INT_MAX - 9) / 10)
                    width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);   /* 음수 정밀도 = 정밀도 없음 */
                if (prec < 0)
                    prec = -1;
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    if (prec <= (INT_MAX - 9) / 10)
                        prec = prec * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }
        if (*fmt == 'h' || *fmt == 'l' || *fmt == 'L')
            len = *fmt++;
        if (flags & F_MINUS)
            flags &= ~F_ZERO;
        if (flags & F_PLUS)
            flags &= ~F_SPACE;

        switch (*fmt) {
        case 'd':
        case 'i': {
            long v = (len == 'l') ? va_arg(ap, long) : (long)va_arg(ap, int);
            unsigned long mag;

            if (len == 'h')
                v = (short)v;
            mag = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
            if (prec >= 0)
                flags &= ~F_ZERO;
            emit_int(&o, mag, v < 0, 10, 0, flags, width, prec, 1, 0);
            break;
        }
        case 'o':
        case 'u':
        case 'x':
        case 'X': {
            unsigned long v = (len == 'l') ? va_arg(ap, unsigned long)
                                           : (unsigned long)va_arg(ap, unsigned int);
            int base = *fmt == 'o' ? 8 : (*fmt == 'u' ? 10 : 16);

            if (len == 'h')
                v = (unsigned short)v;
            if (prec >= 0)
                flags &= ~F_ZERO;
            emit_int(&o, v, 0, base, *fmt == 'X', flags, width, prec, 0, 0);
            break;
        }
        case 'p': {
            void *p = va_arg(ap, void *);

            emit_int(&o, (unsigned long)p, 0, 16, 0, flags & ~F_ZERO, width, prec, 0, 1);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);

            emit_str(&o, &c, 1, flags, width);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            size_t n = 0;

            if (s == NULL)
                s = "(null)";
            /* 정밀도가 있으면 NUL 이 없어도 그 이상 읽지 않는다 */
            while ((prec < 0 || n < (size_t)prec) && s[n] != '\0')
                n++;
            emit_str(&o, s, n, flags, width);
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G': {
            double v = (len == 'L') ? (double)va_arg(ap, long double) : va_arg(ap, double);

            emit_float(&o, v, *fmt, flags, width, prec);
            break;
        }
        case 'n':
            if (len == 'h')
                *va_arg(ap, short *) = (short)o.count;
            else if (len == 'l')
                *va_arg(ap, long *) = (long)o.count;
            else
                *va_arg(ap, int *) = (int)o.count;
            break;
        case '%':
            emit(&o, "%", 1);
            break;
        case '\0':                      /* 서식이 '%' 로 끝났다: 그대로 출력 */
            emit(&o, spec, (size_t)(fmt - spec));
            continue;
        default:                        /* 모르는 변환: 명세를 그대로 */
            emit(&o, spec, (size_t)(fmt + 1 - spec));
            break;
        }
        fmt++;
    }
    if (o.failed || o.count > (unsigned long)INT_MAX)
        return -1;
    return (int)o.count;
}

/* ---------------- 문자열 출력 ---------------- */
struct strbuf {
    char *s;
    size_t cap;     /* NUL 자리 포함 남은 공간 (0 이면 아무것도 쓰지 않는다) */
    int unbounded;  /* sprintf */
};

static int str_sink(void *ctx, const char *s, size_t n)
{
    struct strbuf *b = (struct strbuf *)ctx;

    if (b->unbounded) {
        memcpy(b->s, s, n);
        b->s += n;
        return 0;
    }
    if (b->cap > 1) {
        size_t k = n < b->cap - 1 ? n : b->cap - 1;

        memcpy(b->s, s, k);
        b->s += k;
        b->cap -= k;
    }
    return 0;
}

int vsnprintf(char *s, size_t n, const char *format, va_list arg)
{
    struct strbuf b;
    int r;

    b.s = s;
    b.cap = n;
    b.unbounded = 0;
    r = __nl_vformat(str_sink, &b, format, arg);
    if (n > 0)
        *b.s = '\0';
    return r;
}

int vsprintf(char *s, const char *format, va_list arg)
{
    struct strbuf b;
    int r;

    b.s = s;
    b.cap = 0;
    b.unbounded = 1;
    r = __nl_vformat(str_sink, &b, format, arg);
    *b.s = '\0';
    return r;
}

int snprintf(char *s, size_t n, const char *format, ...)
{
    va_list ap;
    int r;

    va_start(ap, format);
    r = vsnprintf(s, n, format, ap);
    va_end(ap);
    return r;
}

int sprintf(char *s, const char *format, ...)
{
    va_list ap;
    int r;

    va_start(ap, format);
    r = vsprintf(s, format, ap);
    va_end(ap);
    return r;
}

/* ---------------- 스트림 출력 ---------------- */
static int file_sink(void *ctx, const char *s, size_t n)
{
    return fwrite(s, 1, n, (FILE *)ctx) == n ? 0 : -1;
}

int vfprintf(FILE *stream, const char *format, va_list arg)
{
    return __nl_vformat(file_sink, stream, format, arg);
}

int vprintf(const char *format, va_list arg)
{
    return vfprintf(stdout, format, arg);
}

int fprintf(FILE *stream, const char *format, ...)
{
    va_list ap;
    int r;

    va_start(ap, format);
    r = vfprintf(stream, format, ap);
    va_end(ap);
    return r;
}

int printf(const char *format, ...)
{
    va_list ap;
    int r;

    va_start(ap, format);
    r = vfprintf(stdout, format, ap);
    va_end(ap);
    return r;
}
