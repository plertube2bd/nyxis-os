/*
 * math.c - C89 7.5
 *
 * 구현 방침:
 *   - 정확히 계산할 수 있는 것(fabs floor ceil modf frexp ldexp fmod sqrt)은 비트 조작 또는
 *     정확한 명령(fprem, fscale, sqrtsd)으로 — 오차 0 (sqrt 는 IEEE 가 정확한 반올림을 보장).
 *   - 초월 함수는 x87 의 확장 정밀도(64비트 가수) 명령으로 계산한 뒤 double 로 반올림한다.
 *     중간 결과에 double 보다 11비트 많은 정밀도가 있어서 보통 1 ulp 이내다.
 *     (x87 fsin/fcos 의 내부 π 는 66비트라 π 의 큰 배수 근처에서는 상대 오차가 커질 수 있다)
 *   - 오류는 C89 7.5.1: 정의역 오류는 EDOM + NaN, 넘침은 ERANGE + ±HUGE_VAL, 아래 넘침은 ERANGE + 0.
 */
#include <math.h>
#include <errno.h>

union dbits {
    double d;
    unsigned long u;
};

#define SIGN_BIT  0x8000000000000000UL
#define EXP_MASK  0x7FF0000000000000UL
#define FRAC_MASK 0x000FFFFFFFFFFFFFUL

static double nan_val(void)
{
    union dbits b;

    b.u = 0x7FF8000000000000UL;
    return b.d;
}

static int is_nan(double x)
{
    return x != x;
}

static int is_inf(double x)
{
    union dbits b;

    b.d = x;
    return (b.u & ~SIGN_BIT) == EXP_MASK;
}

static double domain_error(void)
{
    errno = EDOM;
    return nan_val();
}

/* 확장 정밀도 결과를 double 로: 넘침/아래 넘침을 errno 로 알린다 */
static double finish(long double r)
{
    double d = (double)r;

    if (is_inf(d)) {
        errno = ERANGE;
        return d < 0 ? -HUGE_VAL : HUGE_VAL;
    }
    if (d == 0.0 && r != 0.0L)
        errno = ERANGE;
    return d;
}

/* ---------------- x87 기본 연산 ---------------- */
static long double x87_frndint(long double x)
{
    long double r;

    __asm__ ("frndint" : "=t"(r) : "0"(x));
    return r;
}

static long double x87_f2xm1(long double x)       /* 2^x - 1, |x| <= 1 */
{
    long double r;

    __asm__ ("f2xm1" : "=t"(r) : "0"(x));
    return r;
}

static long double x87_fscale(long double x, long double n)   /* x * 2^trunc(n) */
{
    long double r;

    __asm__ ("fscale" : "=t"(r) : "0"(x), "u"(n));
    return r;
}

static long double x87_fyl2x(long double x, long double y)    /* y * log2(x) */
{
    long double r;

    __asm__ ("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

static long double x87_fpatan(long double y, long double x)   /* atan2(y, x) */
{
    long double r;

    __asm__ ("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

static long double x87_fprem(long double x, long double y)    /* x - trunc(x/y)*y, 정확 */
{
    long double r;

    __asm__ ("1: fprem\n\t"
             "fnstsw %%ax\n\t"
             "testw $0x400, %%ax\n\t"
             "jnz 1b"
             : "=t"(r) : "0"(x), "u"(y) : "ax", "cc");
    return r;
}

static void memset_u32(unsigned int *p, int n)
{
    while (n--)
        *p++ = 0;
}

static long double x87_fsqrt(long double x)
{
    long double r;

    __asm__ ("fsqrt" : "=t"(r) : "0"(x));
    return r;
}

static long double c_l2e(void) { long double r; __asm__ ("fldl2e" : "=t"(r)); return r; }
static long double c_ln2(void) { long double r; __asm__ ("fldln2" : "=t"(r)); return r; }
static long double c_lg2(void) { long double r; __asm__ ("fldlg2" : "=t"(r)); return r; }
static long double c_pi(void)  { long double r; __asm__ ("fldpi" : "=t"(r)); return r; }

/* 2^t (확장 정밀도). t 가 아주 크거나 작으면 무한대/0 */
static long double exp2l_(long double t)
{
    long double n;

    if (t > 20000.0L)
        t = 20000.0L;
    if (t < -20000.0L)
        t = -20000.0L;
    n = x87_frndint(t);
    return x87_fscale(x87_f2xm1(t - n) + 1.0L, n);
}

/* e^x - 1 (작은 x 에서도 정확하게) */
static long double expm1l_(long double x)
{
    long double t = x * c_l2e();

    if (t >= -1.0L && t <= 1.0L)
        return x87_f2xm1(t);
    return exp2l_(t) - 1.0L;
}

/* ---------------- 7.5.6 정확한 연산 ---------------- */
double fabs(double x)
{
    union dbits b;

    b.d = x;
    b.u &= ~SIGN_BIT;
    return b.d;
}

double modf(double value, double *iptr)
{
    union dbits b;
    int e;

    b.d = value;
    e = (int)((b.u >> 52) & 0x7FF) - 1023;
    if (e >= 52) {                          /* 이미 정수 (또는 inf/nan) */
        *iptr = value;
        if (is_nan(value))
            return value;
        b.u &= SIGN_BIT;                    /* ±0 */
        return b.d;
    }
    if (e < 0) {                            /* |value| < 1 */
        union dbits z;

        z.u = b.u & SIGN_BIT;
        *iptr = z.d;
        return value;
    }
    b.u &= ~(FRAC_MASK >> e);
    *iptr = b.d;
    if (b.d == value) {
        union dbits z;

        z.u = b.u & SIGN_BIT;
        return z.d;
    }
    return value - b.d;                     /* 정확하다 (같은 지수 범위의 뺄셈) */
}

double floor(double x)
{
    double i;
    double f = modf(x, &i);

    return (f < 0.0) ? i - 1.0 : i;
}

double ceil(double x)
{
    double i;
    double f = modf(x, &i);

    return (f > 0.0) ? i + 1.0 : i;
}

double frexp(double value, int *exp)
{
    union dbits b;
    int e;

    *exp = 0;
    if (value == 0.0 || is_nan(value) || is_inf(value))
        return value;
    b.d = value;
    e = (int)((b.u >> 52) & 0x7FF);
    if (e == 0) {                           /* 비정규수: 2^54 를 곱해 정규화 */
        b.d = value * 18014398509481984.0;
        e = (int)((b.u >> 52) & 0x7FF) - 54;
    }
    *exp = e - 1022;
    b.u = (b.u & ~EXP_MASK) | (1022UL << 52);
    return b.d;
}

double ldexp(double x, int exp)
{
    if (x == 0.0 || is_nan(x) || is_inf(x))
        return x;
    return finish(x87_fscale((long double)x, (long double)exp));   /* fscale 은 정확, 반올림은 한 번 */
}

double fmod(double x, double y)
{
    if (is_nan(x) || is_nan(y))
        return nan_val();
    if (y == 0.0 || is_inf(x))
        return domain_error();
    if (is_inf(y))
        return x;
    return (double)x87_fprem((long double)x, (long double)y);
}

double sqrt(double x)
{
    double r;

    if (x < 0.0)
        return domain_error();
    __asm__ ("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

/* ---------------- 7.5.4 지수/로그 ---------------- */
double exp(double x)
{
    if (is_nan(x))
        return x;
    if (is_inf(x))
        return x > 0 ? x : 0.0;
    return finish(exp2l_((long double)x * c_l2e()));
}

double log(double x)
{
    if (is_nan(x))
        return x;
    if (x < 0.0)
        return domain_error();
    if (x == 0.0) {
        errno = ERANGE;
        return -HUGE_VAL;
    }
    if (is_inf(x))
        return x;
    return (double)x87_fyl2x((long double)x, c_ln2());
}

double log10(double x)
{
    if (is_nan(x))
        return x;
    if (x < 0.0)
        return domain_error();
    if (x == 0.0) {
        errno = ERANGE;
        return -HUGE_VAL;
    }
    if (is_inf(x))
        return x;
    return (double)x87_fyl2x((long double)x, c_lg2());
}

/* ---------------- 7.5.5 거듭제곱 ---------------- */
static int is_integer(double y)
{
    double i;

    return modf(y, &i) == 0.0;
}

static int is_odd_integer(double y)
{
    /* |y| >= 2^53 이면 모든 double 이 짝수 */
    if (!is_integer(y) || fabs(y) >= 9007199254740992.0)
        return 0;
    return fmod(y, 2.0) != 0.0;
}

double pow(double x, double y)
{
    int neg = 0;
    long double r;
    long double ax;

    if (y == 0.0)
        return 1.0;
    if (is_nan(x) || is_nan(y))
        return nan_val();
    if (x == 0.0) {
        if (y < 0.0) {                      /* C89: x 가 0 이고 y <= 0 이면 정의역 오류 */
            errno = EDOM;
            return HUGE_VAL;
        }
        return is_odd_integer(y) ? x : 0.0;  /* (-0)^홀수 = -0 */
    }
    if (x < 0.0 && !is_inf(y)) {           /* y 가 무한대면 아래에서 |x| 로 처리 */
        if (!is_integer(y))
            return domain_error();
        neg = is_odd_integer(y);
    }
    ax = fabs(x);
    if (is_inf(y)) {
        if (ax == 1.0L)
            return 1.0;
        return ((ax > 1.0L) == (y > 0.0)) ? HUGE_VAL : 0.0;
    }
    if (is_inf(x)) {
        r = y > 0.0 ? (long double)HUGE_VAL : 0.0L;
        return (double)(neg ? -r : r);
    }

    if (is_integer(y) && fabs(y) <= 64.0) {
        /* 작은 정수 지수: 확장 정밀도에서 제곱을 거듭한다 (pow(10, 22) 같은 값이 정확히 나온다).
         * 제곱할 때마다 상대 오차가 두 배가 되므로 지수가 크면 아래의 로그 경로가 더 정확하다 */
        unsigned long k = (unsigned long)fabs(y);
        long double b = ax;

        r = 1.0L;
        while (k) {
            if (k & 1)
                r *= b;
            b *= b;
            k >>= 1;
        }
        if (y < 0.0)
            r = 1.0L / r;
    } else {
        r = exp2l_(x87_fyl2x(ax, (long double)y));      /* 2^(y * log2|x|) */
    }
    return finish(neg ? -r : r);
}

/* ---------------- 7.5.2 삼각 함수 ---------------- */

/*
 * 삼각 함수의 인자 축소 (Payne-Hanek): x = k * (π/2) + r, |r| <= π/4.
 * x87 fsin/fcos 는 내부 π 가 66비트뿐이라 큰 x 에서 축소 오차가 커진다(1e10 에서 ~1e-10).
 * 그래서 2/π 를 1280비트까지 담은 표로 x * 2/π 의 (mod 4) 부분과 소수부를 정수 연산으로 정확히 구하고,
 * x87 명령은 이미 작아진 r 에만 쓴다. 표는 Machin 공식으로 계산한 2/π 의 이진 전개다.
 */
static const unsigned int two_over_pi[40] = {
    0xA2F9836EU, 0x4E441529U, 0xFC2757D1U, 0xF534DDC0U, 0xDB629599U, 0x3C439041U, 0xFE5163ABU,
    0xDEBBC561U, 0xB7246E3AU, 0x424DD2E0U, 0x06492EEAU, 0x09D1921CU, 0xFE1DEB1CU, 0xB129A73EU,
    0xE88235F5U, 0x2EBB4484U, 0xE99C7026U, 0xB45F7E41U, 0x3991D639U, 0x835339F4U, 0x9C845F8BU,
    0xBDF9283BU, 0x1FF897FFU, 0xDE05980FU, 0xEF2F118BU, 0x5A0A6D1FU, 0x6D367ECFU, 0x27CB09B7U,
    0x4F463F66U, 0x9E5FEA2DU, 0x7527BAC7U, 0xEBE5F17BU, 0x3D0739F7U, 0x8A5292EAU, 0x6BFB5FB1U,
    0x1F8D5D08U, 0x56033046U, 0xFC7B6BABU, 0xF0CFBC20U, 0x9AF4361DU
};

/* 2/π 의 소수점 아래 i 번째 비트 (i >= 1) */
static unsigned int tpi_bit(long i)
{
    if (i < 1 || i > 1280)
        return 0;
    i--;
    return (two_over_pi[i / 32] >> (31 - i % 32)) & 1U;
}

/* |x| >= π/4 인 유한한 x 에 대해 r (확장 정밀도) 을 구하고 사분면 k (0~3) 를 돌려준다 */
static int rem_pio2(double x, long double *r)
{
    union dbits b;
    unsigned long m;
    int e;
    unsigned int w[6];                  /* 창: 2/π 의 비트 [s, s+192), w[5] 가 가장 높은 자리 */
    unsigned int mm[2];
    unsigned int p[8];                  /* m * W (256비트, p[7] 이 가장 높은 자리) */
    long s;
    int i;
    int j;
    int k;
    int neg = 0;
    int top;
    unsigned long mant;
    long double f;

    b.d = fabs(x);
    e = (int)((b.u >> 52) & 0x7FF);
    m = (b.u & FRAC_MASK) | (e ? (1UL << 52) : 0);
    e = (e ? e : 1) - 1075;                         /* |x| = m * 2^e */

    /* x * 2/π = m * 2^e * Σ b_i 2^-i. i <= e-2 인 항은 4 의 배수라 (mod 4) 에서 사라진다.
     * s = e-1 부터 192비트를 W 로 잡으면 (mod 4) 값 = m * W / 2^190:
     * P = m * W 의 비트 190~191 이 사분면, 그 아래 190비트가 소수부 */
    s = (long)e - 1;
    for (i = 0; i < 6; i++) {
        unsigned int v = 0;

        for (j = 0; j < 32; j++)
            v = (v << 1) | tpi_bit(s + (long)(5 - i) * 32 + j);
        w[i] = v;
    }
    mm[0] = (unsigned int)(m & 0xFFFFFFFFU);
    mm[1] = (unsigned int)(m >> 32);
    memset_u32(p, 8);
    for (i = 0; i < 2; i++) {
        unsigned long carry = 0;

        for (j = 0; j < 6; j++) {
            unsigned long t = (unsigned long)mm[i] * w[j] + p[i + j] + carry;

            p[i + j] = (unsigned int)t;
            carry = t >> 32;
        }
        for (j = i + 6; carry && j < 8; j++) {
            unsigned long t = (unsigned long)p[j] + carry;

            p[j] = (unsigned int)t;
            carry = t >> 32;
        }
    }
    k = (int)((p[5] >> 30) & 3U);
    for (i = 5; i > 0; i--)                         /* 소수부를 2비트 올려 192비트 소수로 */
        p[i] = (p[i] << 2) | (p[i - 1] >> 30);
    p[0] <<= 2;

    /* 소수부 p[0..5] 가 1/2 이상이면 다음 사분면에서 음수 쪽으로 (|f| <= 1/2) */
    if (p[5] & 0x80000000U) {
        unsigned long borrow = 1;

        k = (k + 1) & 3;
        neg = 1;
        for (i = 0; i < 6; i++) {                   /* f = 2^192 - frac (2의 보수) */
            unsigned long t = (unsigned long)(unsigned int)~p[i] + borrow;

            p[i] = (unsigned int)t;
            borrow = t >> 32;
        }
    }

    /* 192비트 소수를 long double 로 (가장 높은 1 비트부터 64비트) */
    for (top = 5; top >= 0 && p[top] == 0; top--)
        ;
    if (top < 0) {
        *r = 0.0L;
        return k;
    }
    {
        int bit = 31;
        long pos;

        while (!((p[top] >> bit) & 1U))
            bit--;
        pos = (long)top * 32 + bit;                 /* 가장 높은 1 비트의 위치 (0 = 2^-192) */
        mant = 0;
        for (i = 0; i < 64; i++) {
            long q = pos - i;
            unsigned long bitv = q >= 0 ? (p[q / 32] >> (q % 32)) & 1U : 0;

            mant = (mant << 1) | bitv;
        }
        f = x87_fscale((long double)mant, (long double)(pos - 63 - 192));
    }
    f *= c_pi() / 2.0L;
    *r = neg ? -f : f;
    return k;
}

static long double x87_sin(long double r) { long double v; __asm__ ("fsin" : "=t"(v) : "0"(r)); return v; }
static long double x87_cos(long double r) { long double v; __asm__ ("fcos" : "=t"(v) : "0"(r)); return v; }
static long double x87_tan(long double r)
{
    long double v;

    __asm__ ("fptan\n\tfstp %%st(0)" : "=t"(v) : "0"(r));
    return v;
}

#define PIO4 0.78539816339744830962

double sin(double x)
{
    long double r;
    long double v;

    if (is_nan(x))
        return x;
    if (is_inf(x))
        return domain_error();
    if (fabs(x) < PIO4)
        return (double)x87_sin(x);
    switch (rem_pio2(x, &r)) {
    case 0:  v = x87_sin(r); break;
    case 1:  v = x87_cos(r); break;
    case 2:  v = -x87_sin(r); break;
    default: v = -x87_cos(r); break;
    }
    return (double)(x < 0 ? -v : v);
}

double cos(double x)
{
    long double r;
    long double v;

    if (is_nan(x))
        return x;
    if (is_inf(x))
        return domain_error();
    if (fabs(x) < PIO4)
        return (double)x87_cos(x);
    switch (rem_pio2(x, &r)) {
    case 0:  v = x87_cos(r); break;
    case 1:  v = -x87_sin(r); break;
    case 2:  v = -x87_cos(r); break;
    default: v = x87_sin(r); break;
    }
    return (double)v;
}

double tan(double x)
{
    long double r;
    long double v;

    if (is_nan(x))
        return x;
    if (is_inf(x))
        return domain_error();
    if (fabs(x) < PIO4)
        return (double)x87_tan(x);
    if (rem_pio2(x, &r) & 1)
        v = -1.0L / x87_tan(r);
    else
        v = x87_tan(r);
    return finish(x < 0 ? -v : v);
}

double atan(double x)
{
    if (is_nan(x))
        return x;
    return (double)x87_fpatan((long double)x, 1.0L);
}

double atan2(double y, double x)
{
    if (is_nan(x) || is_nan(y))
        return nan_val();
    return (double)x87_fpatan((long double)y, (long double)x);
}

double asin(double x)
{
    long double lx = x;

    if (is_nan(x))
        return x;
    if (x > 1.0 || x < -1.0)
        return domain_error();
    return (double)x87_fpatan(lx, x87_fsqrt((1.0L - lx) * (1.0L + lx)));
}

double acos(double x)
{
    long double lx = x;

    if (is_nan(x))
        return x;
    if (x > 1.0 || x < -1.0)
        return domain_error();
    return (double)x87_fpatan(x87_fsqrt((1.0L - lx) * (1.0L + lx)), lx);
}

/* ---------------- 7.5.3 쌍곡선 함수 ---------------- */
double sinh(double x)
{
    long double ax = fabs(x);
    long double r;

    if (is_nan(x) || is_inf(x))
        return x;
    if (ax > 22.0L) {
        r = exp2l_(ax * c_l2e() - 1.0L);    /* e^|x| / 2 */
    } else {
        long double a = expm1l_(ax);        /* (a + a/(a+1)) / 2: 작은 x 에서도 정확 */

        r = (a + a / (a + 1.0L)) / 2.0L;
    }
    return finish(x < 0 ? -r : r);
}

double cosh(double x)
{
    long double ax = fabs(x);
    long double e;

    if (is_nan(x))
        return x;
    if (is_inf(x))
        return HUGE_VAL;
    if (ax > 22.0L)
        return finish(exp2l_(ax * c_l2e() - 1.0L));
    e = exp2l_(ax * c_l2e());
    return finish((e + 1.0L / e) / 2.0L);
}

double tanh(double x)
{
    long double ax = fabs(x);
    long double r;

    if (is_nan(x))
        return x;
    if (ax > 22.0L) {
        r = 1.0L;
    } else {
        long double a = expm1l_(2.0L * ax);

        r = a / (a + 2.0L);
    }
    return (double)(x < 0 ? -r : r);
}

/* ---------------- C99: long double ---------------- */
long double ldexpl(long double x, int exp)
{
    long double r;
    long double e = (long double)exp;

    __asm__("fscale" : "=t"(r) : "0"(x), "u"(e));
    return r;
}

long double fabsl(long double x)
{
    return x < 0 ? -x : x;
}
