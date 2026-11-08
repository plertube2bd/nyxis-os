/*
 * stdlib.c - C89 7.10 의 수 변환, 난수, 검색/정렬, 정수 산술, 멀티바이트
 */
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

static int digit_value(int c)
{
    if (isdigit(c))
        return c - '0';
    if (islower(c))
        return c - 'a' + 10;
    if (isupper(c))
        return c - 'A' + 10;
    return 99;
}

/*
 * strtol/strtoul 공통: 부호와 크기를 따로 돌려준다. 크기가 ULONG_MAX 를 넘으면 *overflow.
 * 변환할 수 있는 숫자가 없으면 *endptr = nptr (C89 7.10.1.5).
 */
static unsigned long parse_ul(const char *nptr, char **endptr, int base, int *neg, int *overflow)
{
    const char *s = nptr;
    unsigned long acc = 0;
    int any = 0;
    int d;

    *neg = 0;
    *overflow = 0;
    if (base < 0 || base == 1 || base > 36) {
        errno = EINVAL;
        if (endptr)
            *endptr = (char *)nptr;
        return 0;
    }
    while (isspace((unsigned char)*s))
        s++;
    if (*s == '+' || *s == '-')
        *neg = (*s++ == '-');
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')
        && digit_value((unsigned char)s[2]) < 16) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }
    for (; (d = digit_value((unsigned char)*s)) < base; s++) {
        any = 1;
        if (acc > (ULONG_MAX - (unsigned long)d) / (unsigned long)base)
            *overflow = 1;
        else
            acc = acc * (unsigned long)base + (unsigned long)d;
    }
    if (endptr)
        *endptr = (char *)(any ? s : nptr);
    if (!any)
        *neg = 0;
    return acc;
}

long strtol(const char *nptr, char **endptr, int base)
{
    int neg;
    int overflow;
    unsigned long v = parse_ul(nptr, endptr, base, &neg, &overflow);

    if (neg) {
        if (overflow || v > (unsigned long)LONG_MAX + 1UL) {
            errno = ERANGE;
            return LONG_MIN;
        }
        return (long)(0UL - v);   /* 2의 보수: LONG_MIN 도 정확히 */
    }
    if (overflow || v > (unsigned long)LONG_MAX) {
        errno = ERANGE;
        return LONG_MAX;
    }
    return (long)v;
}

/* C89: '-' 가 붙으면 결과를 부호 없는 형에서 부정한다 */
unsigned long strtoul(const char *nptr, char **endptr, int base)
{
    int neg;
    int overflow;
    unsigned long v = parse_ul(nptr, endptr, base, &neg, &overflow);

    if (overflow) {
        errno = ERANGE;
        return ULONG_MAX;
    }
    return neg ? 0UL - v : v;
}

/* atoi/atol 은 범위를 넘으면 정의되지 않은 동작: 여기서는 strtol 처럼 포화시킨다 */
int atoi(const char *nptr)
{
    long v = strtol(nptr, NULL, 10);

    if (v > INT_MAX)
        return INT_MAX;
    if (v < INT_MIN)
        return INT_MIN;
    return (int)v;
}

long atol(const char *nptr)
{
    return strtol(nptr, NULL, 10);
}

/* 7.10.2: 선형 합동 생성기 (상위 31비트 사용). 암호용이 아니다 */
static unsigned long rand_state = 1;

int rand(void)
{
    rand_state = rand_state * 6364136223846793005UL + 1442695040888963407UL;
    return (int)(rand_state >> 33);
}

void srand(unsigned int seed)
{
    rand_state = seed;
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *))
{
    const char *b = (const char *)base;
    size_t lo = 0;
    size_t hi = nmemb;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const void *p = b + mid * size;
        int c = compar(key, p);

        if (c == 0)
            return (void *)p;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

static void swap_bytes(char *a, char *b, size_t n)
{
    while (n--) {
        char t = *a;

        *a++ = *b;
        *b++ = t;
    }
}

/*
 * qsort: 힙 정렬 (최악 O(n log n), 재귀 없음, 추가 메모리 없음).
 * 표준은 안정 정렬을 요구하지 않는다.
 */
static void sift_down(char *b, size_t root, size_t n, size_t size,
                      int (*compar)(const void *, const void *))
{
    for (;;) {
        size_t child = 2 * root + 1;

        if (child >= n)
            return;
        if (child + 1 < n && compar(b + child * size, b + (child + 1) * size) < 0)
            child++;
        if (compar(b + root * size, b + child * size) >= 0)
            return;
        swap_bytes(b + root * size, b + child * size, size);
        root = child;
    }
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *))
{
    char *b = (char *)base;
    size_t i;

    if (nmemb < 2 || size == 0)
        return;
    for (i = nmemb / 2; i-- > 0;)
        sift_down(b, i, nmemb, size, compar);
    for (i = nmemb - 1; i > 0; i--) {
        swap_bytes(b, b + i * size, size);
        sift_down(b, 0, i, size, compar);
    }
}

int abs(int j)   { return j < 0 ? -j : j; }
long labs(long j) { return j < 0 ? -j : j; }

/* C89 는 음수 나눗셈의 반올림 방향을 구현에 맡기지만 div/ldiv 는 0 쪽으로 자른 몫을 요구한다.
 * C 의 '/' 는 gcc/x86_64 에서 이미 0 쪽으로 자른다. */
div_t div(int numer, int denom)
{
    div_t r;

    r.quot = numer / denom;
    r.rem = numer % denom;
    return r;
}

ldiv_t ldiv(long numer, long denom)
{
    ldiv_t r;

    r.quot = numer / denom;
    r.rem = numer % denom;
    return r;
}

/* 7.10.7~8: "C" 로캘에서 멀티바이트 문자는 바이트 하나, 상태 의존 인코딩 없음 */
int mblen(const char *s, size_t n)
{
    return mbtowc(NULL, s, n);
}

int mbtowc(wchar_t *pwc, const char *s, size_t n)
{
    if (s == NULL)
        return 0;
    if (n == 0)
        return -1;
    if (pwc)
        *pwc = (wchar_t)(unsigned char)*s;
    return *s != '\0';
}

int wctomb(char *s, wchar_t wchar)
{
    if (s == NULL)
        return 0;
    if (wchar < 0 || wchar > 0xFF)
        return -1;
    *s = (char)wchar;
    return 1;
}

size_t mbstowcs(wchar_t *pwcs, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        pwcs[i] = (wchar_t)(unsigned char)s[i];
        if (s[i] == '\0')
            return i;
    }
    return n;
}

size_t wcstombs(char *s, const wchar_t *pwcs, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (pwcs[i] < 0 || pwcs[i] > 0xFF)
            return (size_t)-1;
        s[i] = (char)pwcs[i];
        if (pwcs[i] == 0)
            return i;
    }
    return n;
}

/* ---------------- C99: long long, strtof/strtold, _Exit ---------------- */
__extension__ long long strtoll(const char *nptr, char **endptr, int base)
{
    return strtol(nptr, endptr, base);              /* long 과 long long 은 같은 64비트 */
}

__extension__ unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
    return strtoul(nptr, endptr, base);
}

__extension__ long long atoll(const char *nptr)
{
    return strtol(nptr, NULL, 10);
}

__extension__ long long llabs(long long j)
{
    return j < 0 ? -j : j;
}

float strtof(const char *nptr, char **endptr)
{
    return (float)strtod(nptr, endptr);             /* double 을 거친 두 번 반올림 (드문 경우 1 ulp) */
}

long double strtold(const char *nptr, char **endptr)
{
    return (long double)strtod(nptr, endptr);       /* double 정밀도 */
}

void _Exit(int status)
{
    _exit(status);
}
