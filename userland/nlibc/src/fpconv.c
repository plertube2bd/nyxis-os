/*
 * fpconv.c - double <-> 십진 문자열의 "정확한" 변환 (printf 의 %e %f %g, strtod/atof, scanf)
 *
 * 출력 (__nl_dtoa): 유한한 double 은 m * 2^e (m < 2^53) 이므로 십진으로 정확히 유한한 숫자열이다.
 *   e >= 0 이면 m * 2^e 를, e < 0 이면 m * 5^-e (= 값 * 10^-e) 를 큰 정수로 계산해 숫자열을 얻는다.
 *   반올림은 호출하는 쪽(printf.c)이 이 정확한 숫자열 위에서 한다 — 그래서 결과가 항상 정확하다.
 *
 * 입력 (strtod): 처음 19자리와 십진 지수로 long double 근삿값을 만들어 double 로 바꾼 뒤,
 *   그 후보와 이웃 double 사이의 "중간점" 과 입력값을 큰 정수로 정확히 비교해 필요하면 한 칸씩
 *   옮긴다. 결과는 IEEE 754 가장 가까운 값(동점은 짝수)으로 정확히 반올림된다.
 *   유효 숫자는 NL_DEC_MAX 자리까지 보고, 그 뒤에 0 이 아닌 숫자가 있었다는 사실(sticky)만 남긴다.
 *
 * 큰 정수는 10^9 진법 (출력하기 쉽고, 비교는 진법과 무관하다).
 */
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include "nl_internal.h"

#define BN_BASE 1000000000U
#define BN_MAX  300                 /* 10^2700: strtod 비교의 최악(약 2100자리)보다 넉넉하게 */
#define NL_DEC_MAX 800              /* strtod 가 정확히 보는 유효 숫자 수 */

typedef struct {
    int n;                          /* 쓰는 자리 수 (0 = 값 0) */
    unsigned int d[BN_MAX];         /* 낮은 자리부터 */
} bn;

static void bn_set(bn *a, unsigned long v)
{
    a->n = 0;
    while (v) {
        a->d[a->n++] = (unsigned int)(v % BN_BASE);
        v /= BN_BASE;
    }
}

/* a = a * m + add  (m, add < 2^32) */
static void bn_muladd(bn *a, unsigned long m, unsigned long add)
{
    unsigned long carry = add;
    int i;

    for (i = 0; i < a->n; i++) {
        unsigned long t = (unsigned long)a->d[i] * m + carry;

        a->d[i] = (unsigned int)(t % BN_BASE);
        carry = t / BN_BASE;
    }
    while (carry) {
        if (a->n >= BN_MAX)
            abort();                /* 크기 계산이 틀렸다: 조용히 틀린 값을 내느니 멈춘다 */
        a->d[a->n++] = (unsigned int)(carry % BN_BASE);
        carry /= BN_BASE;
    }
}

static void bn_mul_pow5(bn *a, int k)
{
    for (; k >= 13; k -= 13)
        bn_muladd(a, 1220703125UL, 0);          /* 5^13 */
    if (k > 0) {
        unsigned long m = 1;

        while (k--)
            m *= 5;
        bn_muladd(a, m, 0);
    }
}

static void bn_mul_pow2(bn *a, int k)
{
    for (; k >= 31; k -= 31)
        bn_muladd(a, 1UL << 31, 0);
    if (k > 0)
        bn_muladd(a, 1UL << k, 0);
}

static int bn_cmp(const bn *a, const bn *b)
{
    int i;

    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (i = a->n - 1; i >= 0; i--) {
        if (a->d[i] != b->d[i])
            return a->d[i] < b->d[i] ? -1 : 1;
    }
    return 0;
}

/* 10진 문자열로 (앞의 0 없음). 반환: 길이 */
static int bn_to_dec(const bn *a, char *out)
{
    int len = 0;
    int i;
    int k;
    unsigned int top;
    char tmp[10];

    if (a->n == 0) {
        out[0] = '0';
        return 1;
    }
    top = a->d[a->n - 1];
    k = 0;
    do {
        tmp[k++] = (char)('0' + top % 10);
        top /= 10;
    } while (top);
    while (k)
        out[len++] = tmp[--k];
    for (i = a->n - 2; i >= 0; i--) {
        unsigned int v = a->d[i];

        for (k = 8; k >= 0; k--) {
            out[len + k] = (char)('0' + v % 10);
            v /= 10;
        }
        len += 9;
    }
    return len;
}

union dbits {
    double d;
    unsigned long u;
};

/* 양의 유한 double -> (m, e), 값 = m * 2^e */
static void decompose(double v, unsigned long *m, int *e)
{
    union dbits b;
    int ef;

    b.d = v;
    ef = (int)((b.u >> 52) & 0x7FF);
    *m = b.u & ((1UL << 52) - 1);
    if (ef == 0) {
        *e = -1074;
    } else {
        *m |= 1UL << 52;
        *e = ef - 1075;
    }
}

/*
 * 유한한 |v| 의 정확한 십진 표현: 값 = 0.D1D2...Dn * 10^dexp. 끝의 0 은 떼어낸다.
 * digits 는 NL_DTOA_MAX 바이트 이상. 반환: n (v == 0 이면 0, dexp = 0)
 */
int __nl_dtoa(double v, char *digits, int *dexp)
{
    static bn a;                    /* 스택을 아끼려고 정적 (스레드 없음) */
    unsigned long m;
    int e;
    int n;

    if (v < 0)
        v = -v;
    if (v == 0) {
        *dexp = 0;
        return 0;
    }
    decompose(v, &m, &e);
    bn_set(&a, m);
    if (e >= 0) {
        bn_mul_pow2(&a, e);
        n = bn_to_dec(&a, digits);
        *dexp = n;
    } else {
        bn_mul_pow5(&a, -e);        /* m * 5^-e = v * 10^-e */
        n = bn_to_dec(&a, digits);
        *dexp = n + e;
    }
    while (n > 0 && digits[n - 1] == '0')
        n--;
    return n;
}

/* ---------------- strtod ---------------- */

/* 입력 정보: 값 = D * 10^E (+ sticky: D 뒤에 버린 0 이 아닌 숫자가 있다) */
struct decimal {
    char d[NL_DEC_MAX];
    int nd;
    long e;
    int sticky;
};

/* 입력값과 M * 2^F 를 비교 (-1, 0, 1) */
static int cmp_exact(const struct decimal *in, unsigned long M, int F)
{
    static bn L;
    static bn R;
    int i;
    long p2;

    L.n = 0;
    for (i = 0; i < in->nd; i += 9) {          /* D 를 큰 정수로 (9자리씩) */
        unsigned long chunk = 0;
        unsigned long scale = 1;
        int j;

        for (j = i; j < in->nd && j < i + 9; j++) {
            chunk = chunk * 10 + (unsigned long)(in->d[j] - '0');
            scale *= 10;
        }
        bn_muladd(&L, scale, chunk);
    }
    bn_set(&R, M);
    if (in->e >= 0)
        bn_mul_pow5(&L, (int)in->e);
    else
        bn_mul_pow5(&R, (int)-in->e);
    p2 = in->e - F;                             /* L * 2^E 과 R * 2^F */
    if (p2 > 0)
        bn_mul_pow2(&L, (int)p2);
    else if (p2 < 0)
        bn_mul_pow2(&R, (int)-p2);
    i = bn_cmp(&L, &R);
    if (i == 0 && in->sticky)
        i = 1;
    return i;
}

static long double pow10l_(long e)
{
    long double r = 1.0L;
    long double b = 10.0L;
    unsigned long k = (unsigned long)(e < 0 ? -e : e);

    while (k) {
        if (k & 1)
            r *= b;
        b *= b;
        k >>= 1;
    }
    return e < 0 ? 1.0L / r : r;
}

static double from_bits(unsigned long u)
{
    union dbits b;

    b.u = u;
    return b.d;
}

static unsigned long to_bits(double d)
{
    union dbits b;

    b.d = d;
    return b.u;
}

#define DBL_MAX_BITS 0x7FEFFFFFFFFFFFFFUL
#define INF_BITS     0x7FF0000000000000UL

/* 양의 십진값 -> 가장 가까운 double (넘치면 INF_BITS) */
static double decimal_to_double(const struct decimal *in)
{
    unsigned long first = 0;
    int i;
    int n19 = in->nd < 19 ? in->nd : 19;
    long double guess;
    unsigned long bits;
    int round;

    /* 크기가 범위를 크게 벗어나면 바로 결정 (큰 정수 크기의 상한도 이것이 보장한다) */
    if ((long)in->nd + in->e > 310)
        return from_bits(INF_BITS);
    if ((long)in->nd + in->e < -330)
        return 0.0;

    for (i = 0; i < n19; i++)
        first = first * 10 + (unsigned long)(in->d[i] - '0');
    guess = (long double)first * pow10l_(in->e + (long)(in->nd - n19));
    bits = to_bits((double)guess);
    if (bits >= INF_BITS)
        bits = DBL_MAX_BITS;

    for (round = 0; round < 8; round++) {
        unsigned long m;
        int e;
        int c;

        decompose(from_bits(bits), &m, &e);
        if (bits == 0) {
            m = 0;
            e = -1074;
        }
        /* 위쪽 중간점 (2m+1) * 2^(e-1) */
        c = cmp_exact(in, 2 * m + 1, e - 1);
        if (c > 0 || (c == 0 && (m & 1))) {
            bits++;                             /* 다음 double (DBL_MAX 다음은 무한대) */
            if (bits >= INF_BITS)
                return from_bits(INF_BITS);
            continue;
        }
        if (bits == 0)
            break;
        /* 아래쪽 중간점. m 이 2^52 이고 정규수면 아래 칸의 간격은 절반 */
        if (m == (1UL << 52) && ((bits >> 52) & 0x7FF) > 1)
            c = cmp_exact(in, 4 * m - 1, e - 2);
        else
            c = cmp_exact(in, 2 * m - 1, e - 1);
        if (c < 0 || (c == 0 && (m & 1))) {
            bits--;
            continue;
        }
        break;
    }
    return from_bits(bits);
}

static int match_word(const char *s, const char *w)
{
    while (*w) {
        if (tolower((unsigned char)*s) != *w)
            return 0;
        s++;
        w++;
    }
    return 1;
}

double strtod(const char *nptr, char **endptr)
{
    static struct decimal in;
    const char *s = nptr;
    int neg = 0;
    int any = 0;
    int seen_point = 0;
    long exp10 = 0;
    double r;

    in.nd = 0;
    in.e = 0;
    in.sticky = 0;

    while (isspace((unsigned char)*s))
        s++;
    if (*s == '+' || *s == '-')
        neg = (*s++ == '-');

    /* C99 의 inf/nan 도 받아 준다 */
    if (match_word(s, "inf")) {
        s += match_word(s, "infinity") ? 8 : 3;
        if (endptr)
            *endptr = (char *)s;
        r = from_bits(INF_BITS);
        return neg ? -r : r;
    }
    if (match_word(s, "nan")) {
        if (endptr)
            *endptr = (char *)(s + 3);
        return from_bits(0x7FF8000000000000UL);
    }

    for (;; s++) {
        if (*s == '.' && !seen_point) {
            seen_point = 1;
            continue;
        }
        if (!isdigit((unsigned char)*s))
            break;
        any = 1;
        if (*s == '0' && in.nd == 0) {          /* 앞의 0 */
            if (seen_point)
                exp10--;
            continue;
        }
        if (in.nd < NL_DEC_MAX) {
            in.d[in.nd++] = *s;
            if (seen_point)
                exp10--;
        } else {
            if (*s != '0')
                in.sticky = 1;
            if (!seen_point)
                exp10++;
        }
    }
    if (!any) {
        if (endptr)
            *endptr = (char *)nptr;
        return 0.0;
    }
    if ((*s == 'e' || *s == 'E')
        && (isdigit((unsigned char)s[1])
            || ((s[1] == '+' || s[1] == '-') && isdigit((unsigned char)s[2])))) {
        int eneg = 0;
        long ev = 0;

        s++;
        if (*s == '+' || *s == '-')
            eneg = (*s++ == '-');
        while (isdigit((unsigned char)*s)) {
            if (ev < 100000000L)
                ev = ev * 10 + (*s - '0');
            s++;
        }
        exp10 += eneg ? -ev : ev;
    }
    if (endptr)
        *endptr = (char *)s;

    while (in.nd > 0 && in.d[in.nd - 1] == '0') {   /* 끝의 0 은 지수로 */
        in.nd--;
        exp10++;
    }
    if (in.nd == 0)
        return neg ? -0.0 : 0.0;
    in.e = exp10;

    r = decimal_to_double(&in);
    if (to_bits(r) == INF_BITS) {
        errno = ERANGE;
        r = from_bits(INF_BITS);                    /* HUGE_VAL */
    } else if (r == 0.0) {
        errno = ERANGE;                             /* 아래로 넘침 */
    }
    return neg ? -r : r;
}

double atof(const char *nptr)
{
    return strtod(nptr, (char **)0);
}
