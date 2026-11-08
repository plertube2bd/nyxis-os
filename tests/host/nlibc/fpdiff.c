/*
 * fpdiff.c - nlibc 부동소수점 변환/수학 함수를 glibc 와 비교하는 차분 퍼징 (make test-nlibc 의 두 번째 단계)
 *
 *   printf %e %f %g : glibc 와 문자열이 완전히 같아야 한다 (둘 다 정확한 십진 전개 + 동점은 짝수)
 *   strtod          : glibc 와 비트가 같아야 한다 (둘 다 정확한 반올림). 두 double 의 중간점과 그 ±1 을 일부러 만든다
 *   math            : 정확해야 하는 함수(sqrt floor ceil fmod modf frexp ldexp fabs)는 비트 일치,
 *                     초월 함수는 glibc 결과와의 ulp 차이를 재서 한도(ULP_LIMIT)를 넘으면 실패
 *
 *   fpdiff [반복 수] [씨앗]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

int nl_errno;
int nl_snprintf(char *s, size_t n, const char *fmt, ...);
double nl_strtod(const char *s, char **end);
double nl_sin(double), nl_cos(double), nl_tan(double), nl_asin(double), nl_acos(double), nl_atan(double);
double nl_atan2(double, double), nl_sinh(double), nl_cosh(double), nl_tanh(double);
double nl_exp(double), nl_log(double), nl_log10(double), nl_pow(double, double), nl_sqrt(double);
double nl_floor(double), nl_ceil(double), nl_fabs(double), nl_fmod(double, double);
double nl_modf(double, double *), nl_frexp(double, int *), nl_ldexp(double, int);

/* printf.c 의 file_sink 가 참조한다 (이 프로그램에서는 쓰이지 않는다) */
size_t nl_fwrite(const void *p, size_t s, size_t n, void *f)
{
    (void)p; (void)s; (void)n; (void)f;
    return 0;
}

static unsigned long rng_state = 88172645463325252UL;

static unsigned long rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static double from_bits(unsigned long u) { double d; memcpy(&d, &u, 8); return d; }
static unsigned long to_bits(double d) { unsigned long u; memcpy(&u, &d, 8); return u; }

static double rnd_finite(void)
{
    for (;;) {
        double d = from_bits(rnd());

        if (isfinite(d))
            return d;
    }
}

/* 사람이 쓸 법한 값과 경계 값 */
static double rnd_value(void)
{
    switch (rnd() % 6) {
    case 0: return rnd_finite();
    case 1: return (double)(long)(rnd() % 2000001) / pow(10.0, (double)(rnd() % 12));          /* 0.125, 2.675 류 */
    case 2: return ldexp((double)(2 * (long)(rnd() % 100000) + 1), -(int)(rnd() % 30));       /* 정확한 이진 소수: 동점 */
    case 3: return ((rnd() & 1) ? -1.0 : 1.0) * (double)(rnd() % 100000);
    case 4: return ldexp(1.0, (int)(rnd() % 2098) - 1074);                                    /* 2 의 거듭제곱 */
    default: return from_bits(rnd() & 0x000FFFFFFFFFFFFFUL);                                    /* 비정규수 */
    }
}

static long fails;

static void fail(const char *what, const char *detail)
{
    if (fails < 30)
        printf("FAIL %s: %s\n", what, detail);
    fails++;
}

static void test_printf(long iters)
{
    static const char *convs = "efgEG";
    char fmt[32], a[4096], b[4096], msg[8400];
    long i;

    for (i = 0; i < iters; i++) {
        double v = rnd_value();
        char conv = convs[rnd() % 5];
        int prec = (int)(rnd() % 8 == 0 ? rnd() % 60 : rnd() % 20);
        static const char *flagsets[] = { "", "#", "+", " ", "-", "0", "#0", "+#" };
        const char *fl = flagsets[rnd() % 8];
        int width = (int)(rnd() % 4 == 0 ? rnd() % 30 : 0);

        if (conv == 'f' && fabs(v) > 1e60 && prec > 20)
            prec = 20;
        if (width)
            sprintf(fmt, "%%%s%d.%d%c", fl, width, prec, conv);
        else
            sprintf(fmt, "%%%s.%d%c", fl, prec, conv);
        snprintf(a, sizeof(a), fmt, v);
        nl_snprintf(b, sizeof(b), fmt, v);
        if (strcmp(a, b) != 0) {
            snprintf(msg, sizeof(msg), "printf(\"%s\", %a): glibc \"%s\" nlibc \"%s\"", fmt, v, a, b);
            fail("printf", msg);
        }
    }
}

static void check_strtod(const char *s)
{
    char *e1, *e2;
    char msg[2048];
    double a = strtod(s, &e1);
    double b = nl_strtod(s, &e2);

    if (to_bits(a) != to_bits(b) || e1 - s != e2 - s) {
        snprintf(msg, sizeof(msg), "strtod(\"%.200s\"): glibc %a (+%ld) nlibc %a (+%ld)",
                 s, a, (long)(e1 - s), b, (long)(e2 - s));
        fail("strtod", msg);
    }
}

/* 마지막 숫자를 하나 올리거나 내린다 (중간점 바로 위/아래) */
static void bump(char *s, int dir)
{
    char *p = strchr(s, 'e');
    char *q = p ? p - 1 : s + strlen(s) - 1;

    for (; q >= s; q--) {
        if (*q < '0' || *q > '9')
            continue;
        if (dir > 0) {
            if (*q != '9') { (*q)++; return; }
            *q = '0';
        } else {
            if (*q != '0') { (*q)--; return; }
            *q = '9';
        }
    }
}

static void test_strtod(long iters)
{
    char s[1300];
    long i;

    for (i = 0; i < iters; i++) {
        int kind = (int)(rnd() % 4);

        if (kind == 0) {                                    /* 무작위 숫자열 */
            int nd = 1 + (int)(rnd() % (rnd() % 8 == 0 ? 60 : 20));
            int k = 0, j;
            int point = (int)(rnd() % (unsigned long)(nd + 1));

            if (rnd() & 1)
                s[k++] = '-';
            for (j = 0; j < nd; j++) {
                if (j == point)
                    s[k++] = '.';
                s[k++] = (char)('0' + rnd() % 10);
            }
            k += sprintf(s + k, "e%d", (int)(rnd() % 700) - 360);
            s[k] = '\0';
        } else if (kind == 1) {                             /* 무작위 double 의 짧은 표기 */
            sprintf(s, "%.*g", 1 + (int)(rnd() % 17), rnd_finite());
        } else {                                            /* 두 이웃 double 의 정확한 중간점 (±1) */
            double d = fabs(rnd_value());
            double up = nextafter(d, INFINITY);
            long double mid;

            if (!isfinite(up) || d == 0)
                continue;
            mid = ((long double)d + (long double)up) / 2;  /* 64비트 가수에 정확히 들어간다 */
            sprintf(s, "%.800Le", mid);
            {
                char *e = strchr(s, 'e');
                char *z = e - 1;
                char tail[16];

                strcpy(tail, e);
                while (*z == '0')                           /* 끝의 0 을 지워 길이를 줄인다 */
                    z--;
                strcpy(z + 1, tail);
            }
            if (kind == 3)
                bump(s, (rnd() & 1) ? 1 : -1);
        }
        check_strtod(s);
    }
}

/* ulp 차이 (부호가 같은 유한값). 둘 다 NaN 이면 0 */
static double ulp_diff(double a, double b)
{
    long ia, ib;

    if (isnan(a) && isnan(b))
        return 0;
    if (isnan(a) || isnan(b))
        return 1e300;
    if (a == b)
        return 0;
    ia = (long)to_bits(a);
    ib = (long)to_bits(b);
    if (ia < 0) ia = (long)0x8000000000000000UL - ia;
    if (ib < 0) ib = (long)0x8000000000000000UL - ib;
    return (double)(ia > ib ? ia - ib : ib - ia);
}

#define ULP_LIMIT 1.0

struct fn1 { const char *name; double (*ref)(double); double (*nl)(double); double lo, hi; int bits_exact; };

static double rnd_in(double lo, double hi)
{
    if (rnd() % 8 == 0)
        return rnd_finite();                                /* 범위 밖, 특수 영역도 조금 */
    return lo + (hi - lo) * ((double)(rnd() >> 11) / 9007199254740992.0);
}

static void test_math(long iters)
{
    static const struct fn1 fns[] = {
        { "sin", sin, nl_sin, -10, 10, 0 },     { "cos", cos, nl_cos, -10, 10, 0 },
        { "tan", tan, nl_tan, -10, 10, 0 },     { "asin", asin, nl_asin, -1, 1, 0 },
        { "acos", acos, nl_acos, -1, 1, 0 },    { "atan", atan, nl_atan, -100, 100, 0 },
        { "sinh", sinh, nl_sinh, -30, 30, 0 },  { "cosh", cosh, nl_cosh, -30, 30, 0 },
        { "tanh", tanh, nl_tanh, -10, 10, 0 },  { "exp", exp, nl_exp, -745, 710, 0 },
        { "log", log, nl_log, 0, 1e6, 0 },      { "log10", log10, nl_log10, 0, 1e6, 0 },
        { "sqrt", sqrt, nl_sqrt, 0, 1e6, 1 },   { "floor", floor, nl_floor, -1e6, 1e6, 1 },
        { "ceil", ceil, nl_ceil, -1e6, 1e6, 1 }, { "fabs", fabs, nl_fabs, -1e6, 1e6, 1 },
    };
    unsigned f;
    char msg[512];

    for (f = 0; f < sizeof(fns) / sizeof(fns[0]); f++) {
        double worst = 0, worst_x = 0;
        long i;

        for (i = 0; i < iters; i++) {
            double x = rnd_in(fns[f].lo, fns[f].hi);
            double a = fns[f].ref(x);
            double b = fns[f].nl(x);
            double u = fns[f].bits_exact ? (to_bits(a) == to_bits(b) || (isnan(a) && isnan(b)) ? 0 : 1e300)
                                         : ulp_diff(a, b);

            if (u > worst) {
                worst = u;
                worst_x = x;
            }
        }
        printf("  %-6s max %.0f ulp (x = %a)\n", fns[f].name, worst, worst_x);
        if (worst > (fns[f].bits_exact ? 0 : ULP_LIMIT)) {
            snprintf(msg, sizeof(msg), "%s(%a): glibc %a nlibc %a", fns[f].name, worst_x,
                     fns[f].ref(worst_x), fns[f].nl(worst_x));
            fail("math", msg);
        }
    }

    /* 두 인자 / 정확해야 하는 함수 */
    {
        double worst_pow = 0, wx = 0, wy = 0, worst_atan2 = 0;
        long i;

        for (i = 0; i < iters; i++) {
            double x = rnd_in(0, 100), y = rnd_in(-60, 60), u;
            double fx = rnd_value(), fy = rnd_value();
            double ia, ib, ma, mb;
            int ea, eb, n;

            u = ulp_diff(pow(x, y), nl_pow(x, y));
            if (u > worst_pow) { worst_pow = u; wx = x; wy = y; }
            u = ulp_diff(atan2(fx, fy), nl_atan2(fx, fy));
            if (u > worst_atan2) worst_atan2 = u;

            if (fy != 0 && to_bits(fmod(fx, fy)) != to_bits(nl_fmod(fx, fy))) {
                snprintf(msg, sizeof(msg), "fmod(%a, %a): glibc %a nlibc %a", fx, fy, fmod(fx, fy), nl_fmod(fx, fy));
                fail("math", msg);
            }
            ma = modf(fx, &ia);
            mb = nl_modf(fx, &ib);
            if (to_bits(ma) != to_bits(mb) || to_bits(ia) != to_bits(ib)) {
                snprintf(msg, sizeof(msg), "modf(%a)", fx);
                fail("math", msg);
            }
            if (to_bits(frexp(fx, &ea)) != to_bits(nl_frexp(fx, &eb)) || ea != eb) {
                snprintf(msg, sizeof(msg), "frexp(%a)", fx);
                fail("math", msg);
            }
            n = (int)(rnd() % 2400) - 1200;
            if (to_bits(ldexp(fx, n)) != to_bits(nl_ldexp(fx, n))) {
                snprintf(msg, sizeof(msg), "ldexp(%a, %d): glibc %a nlibc %a", fx, n, ldexp(fx, n), nl_ldexp(fx, n));
                fail("math", msg);
            }
        }
        printf("  %-6s max %.0f ulp (x = %a, y = %a)\n", "pow", worst_pow, wx, wy);
        printf("  %-6s max %.0f ulp\n", "atan2", worst_atan2);
        if (worst_pow > ULP_LIMIT) {
            snprintf(msg, sizeof(msg), "pow(%a, %a): glibc %a nlibc %a", wx, wy, pow(wx, wy), nl_pow(wx, wy));
            fail("math", msg);
        }
        if (worst_atan2 > ULP_LIMIT)
            fail("math", "atan2");
    }
}

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 200000;

    if (argc > 2)
        rng_state ^= (unsigned long)atol(argv[2]) * 0x9E3779B97F4A7C15UL;
    printf("fpdiff: %ld iterations\n", iters);
    test_printf(iters);
    test_strtod(iters);
    test_math(iters / 4);
    if (fails) {
        printf("fpdiff: %ld failure(s)\n", fails);
        return 1;
    }
    printf("fpdiff: OK\n");
    return 0;
}
