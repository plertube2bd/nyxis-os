/*
 * nlibc_test.c - nlibc 동작 검사 (C89, nlibc 만 사용)
 *
 *   nlibctest [임시 디렉터리 접두어]     기본값 tmp:/ (호스트에서는 /tmp/... 처럼 '/' 로 끝나게)
 *
 * 같은 소스를 두 곳에서 돌린다:
 *   호스트: tests/host/nlibc/sys_linux.c 와 링크한 정적 Linux 바이너리 (make test-nlibc)
 *   Nyxis : app:/nlibctest.run (nysh 에서 `nlibctest`)
 * 실패한 검사마다 한 줄을 찍고, 종료 코드는 실패 수 (0 = 모두 통과).
 * 마지막 줄 "atexit 2 1" 은 atexit 처리기가 역순으로 불리고 stdout 이 비워졌다는 증거다 (하네스가 확인).
 */
#define _POSIX_C_SOURCE 200809L     /* POSIX 계층 검사: fileno, kill, setenv, clock_gettime ... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <assert.h>
#include <setjmp.h>
#include <math.h>
#include <float.h>
#include <time.h>
#include <signal.h>
#include <locale.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <spawn.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <termios.h>
#include <sys/ioctl.h>

static int failures;
static int checks;

#define CHECK(cond) check_((cond) != 0, #cond, __LINE__)

static void check_(int ok, const char *what, int line)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL line %d: %s\n", line, what);
    }
}

static void check_fmt(const char *expect, const char *fmt, long a, long b)
{
    char buf[128];
    int n = sprintf(buf, fmt, a, b);

    checks++;
    if (strcmp(buf, expect) != 0 || n != (int)strlen(expect)) {
        failures++;
        printf("FAIL printf(\"%s\"): got \"%s\" (%d), want \"%s\"\n", fmt, buf, n, expect);
    }
}

/* Nyxis 안에서 도는지 (같은 검사가 Linux 호스트에서도 돈다: make test-nlibc) */
static int on_nyxis(void)
{
    struct stat st;

    return stat("info:/version", &st) == 0;
}

static void test_string(void)
{
    char buf[64];
    char tok[] = "  a,b;;c  ";
    char *t;

    CHECK(strlen("") == 0 && strlen("hello") == 5);
    CHECK(strcmp("abc", "abd") < 0 && strcmp("b", "a") > 0 && strcmp("x", "x") == 0);
    CHECK(strcmp("\xff", "a") > 0);                     /* unsigned char 로 비교 */
    CHECK(strncmp("abcX", "abcY", 3) == 0 && strncmp("ab", "abc", 5) < 0);
    CHECK(memcmp("\x01\x80", "\x01\x7f", 2) > 0);

    strcpy(buf, "foo");
    strcat(buf, "bar");
    CHECK(strcmp(buf, "foobar") == 0);
    strncat(buf, "bazzz", 3);
    CHECK(strcmp(buf, "foobarbaz") == 0);

    memset(buf, 'x', sizeof(buf));
    strncpy(buf, "ab", 5);
    CHECK(memcmp(buf, "ab\0\0\0x", 6) == 0);           /* 나머지를 0 으로 채우고 그 너머는 건드리지 않음 */

    strcpy(buf, "0123456789");
    memmove(buf + 2, buf, 5);
    CHECK(memcmp(buf, "0101234789", 10) == 0);
    strcpy(buf, "0123456789");
    memmove(buf, buf + 3, 5);
    CHECK(memcmp(buf, "3456756789", 10) == 0);

    CHECK(strchr("hello", 'l') == (char *)"hello" + 2 || strcmp(strchr("hello", 'l'), "llo") == 0);
    CHECK(strchr("hello", '\0') != NULL && *strchr("hello", '\0') == '\0');
    CHECK(strchr("hello", 'z') == NULL);
    CHECK(strcmp(strrchr("hello", 'l'), "lo") == 0);
    CHECK(memchr("abc", 'c', 2) == NULL && memchr("abc", 'c', 3) != NULL);
    CHECK(strspn("aabbc", "ab") == 4 && strcspn("hello", "lo") == 2);
    CHECK(strcmp(strpbrk("hello", "ol"), "llo") == 0 && strpbrk("abc", "xyz") == NULL);
    CHECK(strcmp(strstr("needle in haystack", "hay"), "haystack") == 0);
    CHECK(strstr("abc", "") != NULL && strstr("abc", "abcd") == NULL);

    t = strtok(tok, " ,;");
    CHECK(t && strcmp(t, "a") == 0);
    t = strtok(NULL, " ,;");
    CHECK(t && strcmp(t, "b") == 0);
    t = strtok(NULL, " ,;");
    CHECK(t && strcmp(t, "c") == 0);
    CHECK(strtok(NULL, " ,;") == NULL);

    CHECK(strxfrm(buf, "abc", sizeof(buf)) == 3 && strcmp(buf, "abc") == 0);
    CHECK(strcoll("a", "b") < 0);
    CHECK(strcmp(strerror(ENOENT), "No such file or directory") == 0);
}

/* stdbool.h / stdint.h (이 검사는 C89 로 컴파일된다: bool 은 unsigned char 대체) */
static void test_stdbool_stdint(void)
{
    bool t = true;
    bool f = false;
    int_least16_t l16 = INT_LEAST16_MAX;
    uint_fast32_t f32 = UINT_FAST32_MAX;
    intmax_t m = INTMAX_C(1) << 62;

    CHECK(t && !f && true == 1 && false == 0 && __bool_true_false_are_defined == 1);
    CHECK(sizeof(int8_t) == 1 && sizeof(int16_t) == 2 && sizeof(int32_t) == 4 && sizeof(int64_t) == 8);
    CHECK(sizeof(intptr_t) == sizeof(void *) && sizeof(uintmax_t) == 8);
    CHECK(l16 == 32767 && f32 == UINT64_MAX && m > 0);
    CHECK(INT_FAST8_MIN == -128 && UINT_LEAST8_MAX == 255 && SIG_ATOMIC_MAX == INT32_MAX && WCHAR_MIN < 0);
    CHECK(UINT32_C(4000000000) > 0 && INT64_C(-1) < 0 && UINT64_C(1) << 63 > 0);
}

static void test_ctype(void)
{
    CHECK(isalpha('a') && isalpha('Z') && !isalpha('1') && !isalpha(EOF));
    CHECK(isdigit('0') && !isdigit('a') && isxdigit('f') && isxdigit('F') && !isxdigit('g'));
    CHECK(isspace(' ') && isspace('\t') && isspace('\v') && !isspace('x'));
    CHECK(ispunct('!') && !ispunct('a') && !ispunct(' '));
    CHECK(isprint(' ') && !isgraph(' ') && iscntrl('\n') && iscntrl(127));
    CHECK(toupper('a') == 'A' && tolower('Q') == 'q' && toupper('1') == '1');
    CHECK(!isalpha(200) && !isprint(200));
}

static void test_strtol(void)
{
    char *end;

    CHECK(strtol("  -123xyz", &end, 10) == -123 && strcmp(end, "xyz") == 0);
    CHECK(strtol("0x1F", NULL, 0) == 31 && strtol("017", NULL, 0) == 15 && strtol("17", NULL, 0) == 17);
    CHECK(strtol("ff", NULL, 16) == 255 && strtol("z", NULL, 36) == 35);
    CHECK(strtol("0x", &end, 16) == 0 && strcmp(end, "x") == 0);   /* "0" 만 변환된다 */
    CHECK(strtol("abc", &end, 10) == 0 && strcmp(end, "abc") == 0);
    CHECK(strtol("+", &end, 10) == 0 && strcmp(end, "+") == 0);

    errno = 0;
    CHECK(strtol("9223372036854775807", NULL, 10) == LONG_MAX && errno == 0);
    CHECK(strtol("-9223372036854775808", NULL, 10) == LONG_MIN && errno == 0);
    CHECK(strtol("9223372036854775808", NULL, 10) == LONG_MAX && errno == ERANGE);
    errno = 0;
    CHECK(strtol("-99999999999999999999", NULL, 10) == LONG_MIN && errno == ERANGE);
    errno = 0;
    CHECK(strtoul("18446744073709551615", NULL, 10) == ULONG_MAX && errno == 0);
    CHECK(strtoul("18446744073709551616", NULL, 10) == ULONG_MAX && errno == ERANGE);
    CHECK(strtoul("-1", NULL, 10) == ULONG_MAX);
    CHECK(atoi("  42") == 42 && atol("-7") == -7);

    {
        div_t d = div(-7, 2);
        ldiv_t l = ldiv(7L, -2L);

        CHECK(d.quot == -3 && d.rem == -1 && l.quot == -3 && l.rem == 1);
        CHECK(abs(-5) == 5 && labs(-5L) == 5L);
    }
}

static void test_printf(void)
{
    char buf[16];
    int n;

    check_fmt("42", "%d", 42, 0);
    check_fmt("-42", "%d", -42, 0);
    check_fmt("[   42]", "[%5d]", 42, 0);
    check_fmt("[42   ]", "[%-5d]", 42, 0);
    check_fmt("[00042]", "[%05d]", 42, 0);
    check_fmt("[-0042]", "[%05d]", -42, 0);
    check_fmt("[  042]", "[%5.3d]", 42, 0);
    check_fmt("[+42]", "[%+d]", 42, 0);
    check_fmt("[ 42]", "[% d]", 42, 0);
    check_fmt("[]", "[%.0d]", 0, 0);
    check_fmt("[    7]", "[%*d]", 5, 7);
    check_fmt("[7    ]", "[%*d]", -5, 7);
    check_fmt("[007]", "[%.*d]", 3, 7);
    check_fmt("-2147483648", "%d", INT_MIN, 0);
    check_fmt("-9223372036854775808", "%ld", LONG_MIN, 0);
    check_fmt("18446744073709551615", "%lu", -1L, 0);
    check_fmt("4294967295", "%u", -1L, 0);
    check_fmt("ff FF", "%x %X", 255, 255);
    check_fmt("0xff 0XFF", "%#x %#X", 255, 255);
    check_fmt("0 0", "%#x %#o", 0, 0);
    check_fmt("17 017", "%o %#o", 15, 15);
    check_fmt("-1 65535", "%hd %hu", 65535, 65535);
    check_fmt("A", "%c", 'A', 0);
    check_fmt("100%", "%d%%", 100, 0);
    check_fmt("0x0", "%p", 0, 0);
    check_fmt("0x1000", "%p", 0x1000, 0);

    sprintf(buf, "%s|%-4s|%4s|%.2s", "a", "b", "c", "xyz");
    CHECK(strcmp(buf, "a|b   |   c|xy") == 0);
    sprintf(buf, "%s", (char *)NULL);
    CHECK(strcmp(buf, "(null)") == 0);
    sprintf(buf, "ab%ncd", &n);
    CHECK(n == 2 && strcmp(buf, "abcd") == 0);

    n = snprintf(buf, 5, "%s", "hello world");
    CHECK(n == 11 && strcmp(buf, "hell") == 0);
    n = snprintf(NULL, 0, "%d", 12345);
    CHECK(n == 5);
    n = snprintf(buf, 1, "abc");
    CHECK(n == 3 && buf[0] == '\0');

}

static void test_malloc(void)
{
    enum { N = 200 };
    static unsigned char *p[N];
    static size_t sz[N];
    unsigned int round;
    int i;
    char *s;

    CHECK(malloc((size_t)-1) == NULL && errno == ENOMEM);
    CHECK(calloc((size_t)1 << 40, (size_t)1 << 40) == NULL);

    s = (char *)calloc(10, 100);
    CHECK(s != NULL);
    if (s) {
        for (i = 0; i < 1000 && s[i] == 0; i++)
            ;
        CHECK(i == 1000);
        CHECK(((unsigned long)s & 15) == 0);
        free(s);
    }
    free(NULL);

    /* 무작위 할당/해제/재할당. 각 블록에 고유한 무늬를 채워 서로 덮어쓰지 않는지 확인 */
    srand(12345);
    for (round = 0; round < 20000; round++) {
        size_t j;

        i = rand() % N;
        if (p[i]) {
            for (j = 0; j < sz[i]; j++) {
                if (p[i][j] != (unsigned char)(i + j)) {
                    CHECK(!"heap block corrupted");
                    return;
                }
            }
        }
        switch (rand() % 3) {
        case 0:
            free(p[i]);
            p[i] = NULL;
            sz[i] = 0;
            break;
        case 1: {
            size_t n = (size_t)(rand() % 4 == 0 ? rand() % 70000 : rand() % 300);
            unsigned char *q = (unsigned char *)realloc(p[i], n);

            if (n == 0) {
                p[i] = NULL;
                sz[i] = 0;
                break;
            }
            CHECK(q != NULL);
            if (!q)
                return;
            p[i] = q;
            for (j = sz[i]; j < n; j++)
                q[j] = (unsigned char)(i + j);
            sz[i] = n;
            break;
        }
        default:
            if (!p[i]) {
                size_t n = (size_t)(rand() % 2000) + 1;

                p[i] = (unsigned char *)malloc(n);
                CHECK(p[i] != NULL && ((unsigned long)p[i] & 15) == 0);
                if (!p[i])
                    return;
                for (j = 0; j < n; j++)
                    p[i][j] = (unsigned char)(i + j);
                sz[i] = n;
            }
            break;
        }
    }
    for (i = 0; i < N; i++)
        free(p[i]);

    /* 모두 해제한 뒤 큰 블록을 다시 받을 수 있어야 한다 (병합 확인) */
    s = (char *)malloc(1024 * 1024);
    CHECK(s != NULL);
    free(s);
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;

    return x < y ? -1 : x > y;
}

static void test_sort(void)
{
    int a[500];
    int i;
    int key;
    int ok = 1;

    srand(7);
    for (i = 0; i < 500; i++)
        a[i] = rand() % 1000 - 500;
    qsort(a, 500, sizeof(int), cmp_int);
    for (i = 1; i < 500; i++)
        ok &= a[i - 1] <= a[i];
    CHECK(ok);
    key = a[123];
    CHECK(bsearch(&key, a, 500, sizeof(int), cmp_int) != NULL);
    key = 9999;
    CHECK(bsearch(&key, a, 500, sizeof(int), cmp_int) == NULL);
    qsort(a, 0, sizeof(int), cmp_int);
}

static void test_stdio(const char *dir)
{
    char path[256];
    char path2[256];
    char line[64];
    FILE *f;
    int i;
    long pos;
    char big[5000];

    sprintf(path, "%snlibc_test.txt", dir);
    sprintf(path2, "%snlibc_test2.txt", dir);
    (void)remove(path);
    (void)remove(path2);

    CHECK(fopen(path, "r") == NULL && errno == ENOENT);

    f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f)
        return;
    for (i = 0; i < 100; i++)
        fprintf(f, "line %d\n", i);
    CHECK(fclose(f) == 0);

    f = fopen(path, "r");
    CHECK(f != NULL);
    if (!f)
        return;
    for (i = 0; i < 100; i++) {
        char want[32];

        sprintf(want, "line %d\n", i);
        if (!fgets(line, sizeof(line), f) || strcmp(line, want) != 0) {
            CHECK(!"fgets content");
            break;
        }
    }
    CHECK(fgets(line, sizeof(line), f) == NULL && feof(f) && !ferror(f));
    CHECK(fgetc(f) == EOF);
    clearerr(f);
    CHECK(!feof(f));

    rewind(f);
    CHECK(fgetc(f) == 'l' && ftell(f) == 1);
    CHECK(ungetc('X', f) == 'X' && ftell(f) == 0 && fgetc(f) == 'X' && fgetc(f) == 'i');
    CHECK(fseek(f, 6, SEEK_SET) == 0 && fgetc(f) == '\n' && fgetc(f) == 'l');
    CHECK(fseek(f, -2, SEEK_CUR) == 0 && fgetc(f) == '\n');
    CHECK(fseek(f, -7, SEEK_END) == 0 && fgets(line, sizeof(line), f) && strcmp(line, "ine 99\n") == 0);
    CHECK(fputc('z', f) == EOF && ferror(f));          /* 읽기 전용 스트림에 쓰기 */
    fclose(f);

    /* "w" 는 기존 내용을 지운다 */
    f = fopen(path, "w");
    CHECK(f != NULL && fputs("short", f) == 0 && fclose(f) == 0);
    /* "a" 는 끝에 붙인다 */
    f = fopen(path, "a");
    CHECK(f != NULL && fputs("+tail", f) == 0 && fclose(f) == 0);
    f = fopen(path, "r");
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(fread(line, 1, sizeof(line), f) == 10 && memcmp(line, "short+tail", 10) == 0 && feof(f));
    fclose(f);

    /* r+ : 읽다가 쓰기 (사이의 fseek 는 C89 가 요구하는 대로) */
    f = fopen(path, "r+");
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(fgetc(f) == 's');
    CHECK(fseek(f, 0, SEEK_CUR) == 0 && fputs("HO", f) == 0);
    CHECK(fseek(f, 0, SEEK_SET) == 0 && fread(line, 1, 10, f) == 10 && memcmp(line, "sHOrt+tail", 10) == 0);
    fclose(f);

    /* 버퍼보다 큰 쓰기/읽기 */
    for (i = 0; i < (int)sizeof(big); i++)
        big[i] = (char)('a' + i % 26);
    f = fopen(path2, "w+");
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(fwrite(big, 1, 3, f) == 3 && fwrite(big + 3, 1, sizeof(big) - 3, f) == sizeof(big) - 3);
    pos = ftell(f);
    CHECK(pos == (long)sizeof(big));
    rewind(f);
    memset(big, 0, sizeof(big));
    CHECK(fread(big, 1, 10, f) == 10 && fread(big + 10, 1, sizeof(big) - 10, f) == sizeof(big) - 10);
    for (i = 0; i < (int)sizeof(big) && big[i] == (char)('a' + i % 26); i++)
        ;
    CHECK(i == (int)sizeof(big));
    CHECK(fclose(f) == 0);

    CHECK(remove(path) == 0);
    CHECK(rename(path2, path) == 0);
    CHECK(fopen(path2, "r") == NULL && errno == ENOENT);
    f = fopen(path, "r");
    CHECK(f != NULL && fgetc(f) == 'a');
    if (f)
        fclose(f);
    CHECK(remove(path) == 0);
    CHECK(fopen(path, "r") == NULL);

    CHECK(fopen(path, "q") == NULL && errno == EINVAL);
}

static void test_scanf(const char *dir)
{
    int a = -1;
    int b = -1;
    long l = 0;
    unsigned int u = 0;
    unsigned long ul = 0;
    short h = 0;
    char s1[32];
    char s2[32];
    char c3[4];
    int n = -1;
    void *p = NULL;
    char path[256];
    FILE *f;

    CHECK(sscanf("42 -17", "%d %d", &a, &b) == 2 && a == 42 && b == -17);
    CHECK(sscanf("  7", "%d", &a) == 1 && a == 7);
    CHECK(sscanf("0x1f 017 99", "%i %i %i", &a, &b, &n) == 3 && a == 31 && b == 15 && n == 99);
    CHECK(sscanf("ff 0XFF 777", "%x %X %o", &a, &b, &u) == 3 && a == 255 && b == 255 && u == 511);
    CHECK(sscanf("-9223372036854775808 18446744073709551615", "%ld %lu", &l, &ul) == 2
          && l == LONG_MIN && ul == ULONG_MAX);
    CHECK(sscanf("4294967295 -2", "%u %hd", &u, &h) == 2 && u == 4294967295U && h == -2);
    CHECK(sscanf("12345", "%3d%d", &a, &b) == 2 && a == 123 && b == 45);   /* 너비 */
    CHECK(sscanf("1 2", "%*d %d", &a) == 1 && a == 2);                    /* 억제 */

    /* 실패와 EOF */
    a = 5;
    CHECK(sscanf("abc", "%d", &a) == 0 && a == 5);
    CHECK(sscanf("", "%d", &a) == EOF);
    CHECK(sscanf("   ", "%d", &a) == EOF);
    CHECK(sscanf("1", "%d %d", &a, &b) == 1);
    CHECK(sscanf("1,2", "%d;%d", &a, &b) == 1);
    CHECK(sscanf("1,2", "%d,%d", &a, &b) == 2 && b == 2);
    CHECK(sscanf("-x", "%d", &a) == 0);
    CHECK(sscanf("0xg", "%x", &a) == 0);                                  /* "0x" 는 완전한 수가 아니다 */

    /* 문자열과 문자 */
    CHECK(sscanf("  hello world", "%s %s", s1, s2) == 2 && strcmp(s1, "hello") == 0 && strcmp(s2, "world") == 0);
    CHECK(sscanf("abcdef", "%3s%s", s1, s2) == 2 && strcmp(s1, "abc") == 0 && strcmp(s2, "def") == 0);
    CHECK(sscanf(" xyz", "%c", c3) == 1 && c3[0] == ' ');                   /* %c 는 공백을 건너뛰지 않는다 */
    CHECK(sscanf("xyz", "%3c", c3) == 1 && memcmp(c3, "xyz", 3) == 0);
    CHECK(sscanf("xy", "%3c", c3) == 0);

    /* 집합 */
    CHECK(sscanf("abc123def", "%[a-z]%[0-9]", s1, s2) == 2 && strcmp(s1, "abc") == 0 && strcmp(s2, "123") == 0);
    CHECK(sscanf("key=value;rest", "%[^=]=%[^;]", s1, s2) == 2 && strcmp(s1, "key") == 0 && strcmp(s2, "value") == 0);
    CHECK(sscanf("]]x", "%[]]", s1) == 1 && strcmp(s1, "]]") == 0);
    CHECK(sscanf("a-b", "%[-ab]", s1) == 1 && strcmp(s1, "a-b") == 0);
    CHECK(sscanf("123", "%[a-z]", s1) == 0);
    CHECK(sscanf("abcdef", "%2[a-z]", s1) == 1 && strcmp(s1, "ab") == 0);

    /* %n, %%, %p */
    CHECK(sscanf("  12abc", "%d%n", &a, &n) == 1 && n == 4);
    CHECK(sscanf("100%", "%d%%", &a) == 1 && a == 100);
    CHECK(sscanf("0x1000", "%p", &p) == 1 && p == (void *)0x1000);
    sprintf(s1, "%p", (void *)&a);
    CHECK(sscanf(s1, "%p", &p) == 1 && p == (void *)&a);

    /* 부동소수점 */
    {
        float fv = 0;
        double dv = 0;
        long double lv = 0;

        CHECK(sscanf("1 2.5 -3e2 .25", "%d %f %lf %Lf", &a, &fv, &dv, &lv) == 4
              && a == 1 && fv == 2.5f && dv == -300.0 && lv == 0.25L);
        CHECK(sscanf("1.5e+x", "%lf", &dv) == 0);      /* 지수 표시 뒤에 숫자가 없다 */
        CHECK(sscanf("12.75abc", "%4lf%s", &dv, s1) == 2 && dv == 12.7 && strcmp(s1, "5abc") == 0);
        CHECK(sscanf("0.1", "%lf", &dv) == 1 && dv == 0.1);
        CHECK(sscanf(".", "%lf", &dv) == 0);
    }

    /* fscanf: 스트림에서 읽고, 읽지 않은 글자는 스트림에 남는다 */
    sprintf(path, "%snlibc_scanf.txt", dir);
    f = fopen(path, "w+");
    CHECK(f != NULL);
    if (!f)
        return;
    fputs("10 20 name:alice\nnext", f);
    rewind(f);
    CHECK(fscanf(f, "%d %d name:%s", &a, &b, s1) == 3 && a == 10 && b == 20 && strcmp(s1, "alice") == 0);
    CHECK(fgetc(f) == '\n' && fscanf(f, "%s", s2) == 1 && strcmp(s2, "next") == 0);
    CHECK(fscanf(f, "%d", &a) == EOF && feof(f));
    fclose(f);
    (void)remove(path);
}

/* ---------------- 부동소수점 ---------------- */
static unsigned long dbits(double d)
{
    union { double d; unsigned long u; } b;

    b.d = d;
    return b.u;
}

static double from_dbits(unsigned long u)
{
    union { double d; unsigned long u; } b;

    b.u = u;
    return b.d;
}

static int near(double a, double b, double rel)
{
    double diff = a > b ? a - b : b - a;
    double mag = fabs(b) > 1.0 ? fabs(b) : 1.0;

    return diff <= rel * mag;
}

static void check_ffmt(const char *expect, const char *fmt, double v)
{
    char buf[512];
    int n = sprintf(buf, fmt, v);

    checks++;
    if (strcmp(buf, expect) != 0 || n != (int)strlen(expect)) {
        failures++;
        printf("FAIL printf(\"%s\"): got \"%s\" (%d), want \"%s\"\n", fmt, buf, n, expect);
    }
}

static void test_float_printf(void)
{
    char buf[64];

    check_ffmt("3.141590", "%f", 3.14159);
    check_ffmt("2.67", "%.2f", 2.675);                 /* 2.675 는 실제로 2.67499999... */
    check_ffmt("0", "%.0f", 0.5);                      /* 동점은 짝수 */
    check_ffmt("2", "%.0f", 1.5);
    check_ffmt("2", "%.0f", 2.5);
    check_ffmt("1", "%.0f", 0.6);
    check_ffmt("0.125", "%.3f", 0.125);
    check_ffmt("0.12", "%.2f", 0.125);                 /* 정확히 중간: 짝수 쪽 */
    check_ffmt("1.234568e+04", "%e", 12345.678);
    check_ffmt("0.000e+00", "%.3e", 0.0);
    check_ffmt("2e+01", "%.0e", 15.0);
    check_ffmt("1.0E-10", "%.1E", 1e-10);
    check_ffmt("100000", "%g", 100000.0);
    check_ffmt("1e+06", "%g", 1e6);
    check_ffmt("0.0001", "%g", 0.0001);
    check_ffmt("1e-05", "%g", 0.00001);
    check_ffmt("3", "%g", 3.0);
    check_ffmt("3.00000", "%#g", 3.0);
    check_ffmt("0", "%g", 0.0);
    check_ffmt("0.000123", "%.3g", 0.0001234);
    check_ffmt("1.23457e+08", "%g", 123456789.0);
    check_ffmt("1E+100", "%G", 1e100);
    check_ffmt("    -1.500", "%10.3f", -1.5);
    check_ffmt("1.5e+00   |", "%-10.1e|", 1.5);
    check_ffmt("+1.0", "%+.1f", 1.0);
    check_ffmt(" 1.0", "% .1f", 1.0);
    check_ffmt("-000003.14", "%010.2f", -3.14159);
    check_ffmt("1.", "%#.0f", 1.0);
    check_ffmt(" 10.0", "%5.1f", 9.96);
    check_ffmt("100000000000000000000.000000", "%f", 1e20);
    check_ffmt("0.10000000000000000555", "%.20f", 0.1);
    check_ffmt("0.10000000000000001", "%.17g", 0.1);
    check_ffmt("-0.000000", "%f", -0.0);
    check_ffmt("1.797693e+308", "%e", DBL_MAX);
    check_ffmt("2.22507e-308", "%g", DBL_MIN);
    check_ffmt("4.9406564584124654e-324", "%.17g", from_dbits(1));
    check_ffmt("inf", "%f", HUGE_VAL);
    check_ffmt("  -INF", "%6E", -HUGE_VAL);
    check_ffmt("nan", "%g", strtod("nan", NULL));
    check_ffmt("179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368.000000", "%f", DBL_MAX);

    sprintf(buf, "%Lf|%d", (long double)1.5, 7);       /* long double 과 뒤따르는 정수 */
    CHECK(strcmp(buf, "1.500000|7") == 0);
    sprintf(buf, "%d %.1f %d %.1f %d", 1, 2.0, 3, 4.0, 5);   /* 정수/실수 인자 섞기 */
    CHECK(strcmp(buf, "1 2.0 3 4.0 5") == 0);
}

static void test_strtod(void)
{
    char *end;
    int i;
    double v;
    int ok = 1;

    CHECK(strtod("3.14", NULL) == 3.14 && atof("  -2.5e3") == -2500.0);
    CHECK(strtod(".5e1", NULL) == 5.0 && strtod("1.", NULL) == 1.0);
    v = strtod("1e", &end);
    CHECK(v == 1.0 && strcmp(end, "e") == 0);
    v = strtod("0x10", &end);
    CHECK(v == 0.0 && strcmp(end, "x10") == 0);
    v = strtod("abc", &end);
    CHECK(v == 0.0 && strcmp(end, "abc") == 0);
    v = strtod("-0.0", NULL);
    CHECK(v == 0.0 && dbits(v) == 0x8000000000000000UL);
    errno = 0;
    CHECK(strtod("1e309", NULL) == HUGE_VAL && errno == ERANGE);
    errno = 0;
    CHECK(strtod("-1e309", NULL) == -HUGE_VAL && errno == ERANGE);
    errno = 0;
    CHECK(strtod("1e-400", NULL) == 0.0 && errno == ERANGE);
    errno = 0;
    CHECK(strtod("1.7976931348623157e308", NULL) == DBL_MAX && errno == 0);
    CHECK(strtod("1.7976931348623159e308", NULL) == HUGE_VAL);      /* 중간점 위: 무한대 */
    CHECK(dbits(strtod("4.9406564584124654e-324", NULL)) == 1);
    CHECK(dbits(strtod("2.4703282292062328e-324", NULL)) == 1);     /* 중간점 바로 위 */
    CHECK(dbits(strtod("2.4703282292062327e-324", NULL)) == 0);     /* 중간점 바로 아래 */
    CHECK(dbits(strtod("2.2250738585072011e-308", NULL)) == 0x000FFFFFFFFFFFFFUL);  /* 유명한 어려운 값 */
    CHECK(dbits(strtod("2.2250738585072012e-308", NULL)) == 0x0010000000000000UL);
    CHECK(strtod("9007199254740993", NULL) == 9007199254740992.0);  /* 동점: 짝수 */
    CHECK(strtod("9007199254740995", NULL) == 9007199254740996.0);
    CHECK(strtod("9007199254740993.0000000000000000000000000001", NULL) == 9007199254740994.0);
    CHECK(strtod("0.1000000000000000055511151231257827021181583404541015625", NULL) == 0.1);
    CHECK(strtod("1e23", NULL) == 1e23 && strtod("8.533e+68", NULL) == 8.533e+68);
    CHECK(strtod("inf", NULL) == HUGE_VAL && strtod("-Infinity", NULL) == -HUGE_VAL);
    v = strtod("nan", NULL);
    CHECK(v != v);

    /* 무작위 비트 무늬: %.17g 로 쓰고 strtod 로 읽으면 같은 비트가 나와야 한다 */
    srand(99);
    for (i = 0; i < 3000 && ok; i++) {
        unsigned long u = ((unsigned long)(unsigned int)rand() << 33) ^ ((unsigned long)(unsigned int)rand() << 11)
                          ^ (unsigned long)(unsigned int)rand();
        char buf[64];

        if (((u >> 52) & 0x7FF) == 0x7FF)
            continue;
        sprintf(buf, "%.17g", from_dbits(u));
        if (dbits(strtod(buf, NULL)) != u) {
            printf("FAIL round trip %016lx -> %s\n", u, buf);
            ok = 0;
        }
        sprintf(buf, "%.16e", from_dbits(u));
        if (dbits(strtod(buf, NULL)) != u) {
            printf("FAIL round trip %016lx -> %s\n", u, buf);
            ok = 0;
        }
    }
    CHECK(ok);
}

static void test_math(void)
{
    double ip;
    int e;
    const double pi = 3.14159265358979323846;

    CHECK(sqrt(16.0) == 4.0 && sqrt(2.0) == 1.4142135623730951 && sqrt(0.0) == 0.0);
    errno = 0;
    CHECK(sqrt(-1.0) != sqrt(-1.0) && errno == EDOM);
    CHECK(fabs(-3.5) == 3.5 && dbits(fabs(-0.0)) == 0);
    CHECK(floor(-1.5) == -2.0 && ceil(-1.5) == -1.0 && floor(2.0) == 2.0 && ceil(2.000001) == 3.0);
    CHECK(floor(1e300) == 1e300 && ceil(-0.5) == 0.0 && dbits(ceil(-0.5)) == 0x8000000000000000UL);
    CHECK(modf(3.75, &ip) == 0.75 && ip == 3.0);
    CHECK(modf(-3.75, &ip) == -0.75 && ip == -3.0);
    CHECK(frexp(8.0, &e) == 0.5 && e == 4);
    CHECK(frexp(from_dbits(1), &e) == 0.5 && e == -1073);
    CHECK(ldexp(0.5, 4) == 8.0 && ldexp(1.0, -1074) == from_dbits(1));
    errno = 0;
    CHECK(ldexp(1.0, 2000) == HUGE_VAL && errno == ERANGE);
    CHECK(fmod(10.0, 3.0) == 1.0 && fmod(-10.0, 3.0) == -1.0 && fmod(5.5, 2.0) == 1.5);
    CHECK(fmod(1e300, 3.0) == 1e300 - 3.0 * floor(1e300 / 3.0) || fmod(1e300, 3.0) < 3.0);
    errno = 0;
    CHECK(fmod(1.0, 0.0) != fmod(1.0, 0.0) && errno == EDOM);

    CHECK(near(sin(pi / 6), 0.5, 1e-15) && cos(0.0) == 1.0 && near(tan(pi / 4), 1.0, 1e-15));
    CHECK(near(sin(1e10), -0.4875060250875107, 1e-15) && near(cos(1e22), 0.5232147853951389, 1e-15));
    CHECK(near(atan(1.0) * 4, pi, 1e-15) && near(atan2(1.0, -1.0), 3 * pi / 4, 1e-15));
    CHECK(atan2(0.0, -1.0) == pi && atan2(-0.0, 1.0) == 0.0);
    CHECK(near(asin(1.0), pi / 2, 1e-15) && near(acos(-1.0), pi, 1e-15) && acos(1.0) == 0.0);
    errno = 0;
    CHECK(asin(2.0) != asin(2.0) && errno == EDOM);
    errno = 0;
    CHECK(sin(HUGE_VAL) != sin(HUGE_VAL) && errno == EDOM);

    CHECK(exp(0.0) == 1.0 && near(exp(1.0), 2.718281828459045, 1e-16) && near(exp(-1.0), 0.36787944117144233, 1e-16));
    errno = 0;
    CHECK(exp(1000.0) == HUGE_VAL && errno == ERANGE);
    errno = 0;
    CHECK(exp(-1000.0) == 0.0 && errno == ERANGE);
    CHECK(near(log(2.718281828459045), 1.0, 1e-16) && log(1.0) == 0.0 && log10(1000.0) == 3.0);
    CHECK(near(log10(2.0), 0.3010299956639812, 1e-16));
    errno = 0;
    CHECK(log(0.0) == -HUGE_VAL && errno == ERANGE);
    errno = 0;
    CHECK(log(-1.0) != log(-1.0) && errno == EDOM);

    CHECK(pow(2.0, 10.0) == 1024.0 && pow(10.0, 22.0) == 1e22 && pow(-2.0, 3.0) == -8.0);
    CHECK(pow(2.0, -2.0) == 0.25 && pow(4.0, 0.5) == 2.0 && near(pow(2.0, 0.5), 1.4142135623730951, 1e-16));
    CHECK(pow(0.0, 0.0) == 1.0 && pow(0.0, 2.0) == 0.0 && near(pow(1.0000001, 1e9), 2.6881038582144647e43, 1e-12));
    errno = 0;
    CHECK(pow(-8.0, 1.0 / 3) != pow(-8.0, 1.0 / 3) && errno == EDOM);
    errno = 0;
    CHECK(pow(10.0, 400.0) == HUGE_VAL && errno == ERANGE);

    CHECK(sinh(0.0) == 0.0 && near(sinh(1e-10), 1e-10, 1e-15) && near(sinh(1.0), 1.1752011936438014, 1e-15));
    CHECK(near(sinh(-30.0), -5343237290762.231, 1e-15));
    CHECK(cosh(0.0) == 1.0 && near(cosh(2.0), 3.7621956910836314, 1e-15));
    CHECK(tanh(100.0) == 1.0 && near(tanh(0.5), 0.46211715726000974, 1e-15) && near(tanh(-1e-20), -1e-20, 1e-15));
    errno = 0;
    CHECK(cosh(1000.0) == HUGE_VAL && errno == ERANGE);

    CHECK(FLT_RADIX == 2 && DBL_MANT_DIG == 53 && DBL_DIG == 15 && 1.0 + DBL_EPSILON != 1.0);
    CHECK(1.0 + DBL_EPSILON / 2 == 1.0 && FLT_MAX > 3e38f && LDBL_MANT_DIG == 64);
}

/* ---------------- time / signal / locale ---------------- */
static void test_time(void)
{
    time_t t = 0;
    struct tm *tm;
    struct tm m;
    char buf[128];
    clock_t c0;

    tm = gmtime(&t);
    CHECK(tm && tm->tm_year == 70 && tm->tm_mon == 0 && tm->tm_mday == 1 && tm->tm_wday == 4 && tm->tm_yday == 0);
    CHECK(strcmp(asctime(tm), "Thu Jan  1 00:00:00 1970\n") == 0);
    t = -1;
    tm = gmtime(&t);
    CHECK(tm && tm->tm_year == 69 && tm->tm_mon == 11 && tm->tm_mday == 31 && tm->tm_hour == 23
          && tm->tm_sec == 59 && tm->tm_wday == 3 && tm->tm_yday == 364);
    t = 951782400L;                                 /* 2000-02-29 (400 의 배수 해의 윤일) */
    tm = gmtime(&t);
    CHECK(tm && tm->tm_year == 100 && tm->tm_mon == 1 && tm->tm_mday == 29 && tm->tm_wday == 2 && tm->tm_yday == 59);
    t = 4107542400L;                                /* 2100-03-01 (2100 은 윤년 아님) */
    tm = gmtime(&t);
    CHECK(tm && tm->tm_year == 200 && tm->tm_mon == 2 && tm->tm_mday == 1);

    memset(&m, 0, sizeof(m));                       /* 2000-01-31 + 1 개월 + 40 일 30 시간 -> 정규화 */
    m.tm_year = 100;
    m.tm_mon = 1;
    m.tm_mday = 31 + 40;
    m.tm_hour = 30;
    t = mktime(&m);
    CHECK(t == 955519200L);
    CHECK(m.tm_year == 100 && m.tm_mon == 3 && m.tm_mday == 12 && m.tm_hour == 6 && m.tm_wday == 3);
    memset(&m, 0, sizeof(m));
    m.tm_year = 70;
    m.tm_mon = -1;                                  /* 1969-12 */
    m.tm_mday = 1;
    CHECK(mktime(&m) == -31L * 86400L && m.tm_year == 69 && m.tm_mon == 11);

    t = 1700000000L;                                /* 2023-11-14 22:13:20 UTC, 화요일 */
    CHECK(strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %a %A %b %B %j %p %I %w %y %Z %%", gmtime(&t)) > 0);
    CHECK(strcmp(buf, "2023-11-14 22:13:20 Tue Tuesday Nov November 318 PM 10 2 23 UTC %") == 0);
    CHECK(strftime(buf, sizeof(buf), "%c|%x|%X|%U|%W", gmtime(&t)) > 0);
    CHECK(strcmp(buf, "Tue Nov 14 22:13:20 2023|11/14/23|22:13:20|46|46") == 0);
    CHECK(strftime(buf, 5, "%Y-%m", gmtime(&t)) == 0);           /* 자리가 모자라면 0 */
    CHECK(strcmp(ctime(&t), "Tue Nov 14 22:13:20 2023\n") == 0);

    CHECK(difftime(10, 3) == 7.0);
    c0 = clock();
    CHECK(c0 >= 0);
    {
        volatile long spin;

        for (spin = 0; spin < 20000000L; spin++)
            ;
    }
    CHECK(clock() >= c0);
    t = time(NULL);
    CHECK(t == (time_t)-1 || t > 1600000000L);      /* 실시간 시계가 없으면 -1 */
}

static volatile sig_atomic_t got_sig;
static void on_sig(int sig) { got_sig = sig; }

/* ---------------- POSIX 신호 (커널 신호: Linux 호스트와 Nyxis 에서 같은 검사) ---------------- */
static volatile sig_atomic_t s_count;
static volatile int s_signo, s_code, s_pid_ok;
static volatile double s_fsink;
static jmp_buf s_jb;
static void *volatile s_addr;

static void on_info(int sig, siginfo_t *info, void *ctx)
{
    (void)ctx;
    s_count++;
    s_signo = info->si_signo;
    s_code = info->si_code;
    s_pid_ok = (info->si_pid == getpid());
    (void)sig;
}

static void on_count(int sig)
{
    volatile double x = 1.5;
    int i;

    (void)sig;
    for (i = 0; i < 50; i++)
        x = x * 1.25 - 0.375;           /* 처리기가 FPU/SSE 레지스터를 어지럽혀도 끊긴 쪽은 그대로여야 한다 */
    s_fsink = x;
    s_count++;
}

static void on_fault(int sig, siginfo_t *info, void *ctx)
{
    (void)ctx;
    s_signo = sig;
    s_addr = info->si_addr;
    longjmp(s_jb, 1);
}

static void msleep(long ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

static int set_handler(int sig, void (*h)(int), int flags)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h;
    sa.sa_flags = flags;
    return sigaction(sig, &sa, NULL);
}

static void test_sigaction(void)
{
    struct sigaction sa;
    struct sigaction old;
    sigset_t set;
    sigset_t cur;
    pid_t pid;
    int st;
    int i;
    char ch;
    int p[2];

    /* sigset */
    CHECK(sigemptyset(&set) == 0 && sigismember(&set, SIGUSR1) == 0);
    CHECK(sigaddset(&set, SIGUSR1) == 0 && sigismember(&set, SIGUSR1) == 1 && sigismember(&set, SIGUSR2) == 0);
    CHECK(sigdelset(&set, SIGUSR1) == 0 && set == 0);
    CHECK(sigfillset(&set) == 0 && sigismember(&set, SIGTERM) == 1);
    CHECK(sigaddset(&set, 0) == -1 && errno == EINVAL && sigismember(&set, NSIG) == -1);

    /* SA_SIGINFO 처리기: 번호, 코드, 보낸 pid. 처리기는 남는다 (두 번) */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_info;
    sa.sa_flags = SA_SIGINFO;
    CHECK(sigaction(SIGUSR1, &sa, &old) == 0 && old.sa_handler == SIG_DFL);
    s_count = 0;
    CHECK(raise(SIGUSR1) == 0 && s_count == 1 && s_signo == SIGUSR1 && s_code == SI_USER && s_pid_ok);
    CHECK(kill(getpid(), SIGUSR1) == 0 && s_count == 2);
    CHECK(sigaction(SIGUSR1, NULL, &old) == 0 && old.sa_sigaction == on_info && (old.sa_flags & SA_SIGINFO));
    CHECK(sigaction(SIGKILL, &sa, NULL) == -1 && errno == EINVAL);
    CHECK(sigaction(SIGSTOP, &sa, NULL) == -1);

    /* 막으면 대기하고, 풀면 한 번 전달된다 */
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    s_count = 0;
    CHECK(sigprocmask(SIG_BLOCK, &set, NULL) == 0);
    CHECK(raise(SIGUSR1) == 0 && raise(SIGUSR1) == 0 && s_count == 0);
    CHECK(sigpending(&cur) == 0 && sigismember(&cur, SIGUSR1) == 1);
    CHECK(sigprocmask(SIG_UNBLOCK, &set, &cur) == 0 && sigismember(&cur, SIGUSR1) == 1);
    CHECK(s_count == 1);                 /* 같은 신호 둘은 하나로 합쳐진다 */
    CHECK(sigpending(&cur) == 0 && sigismember(&cur, SIGUSR1) == 0);

    /* sigsuspend: 막혀 있던 신호를 받고 -1 + EINTR, 막은 집합은 돌아온다 */
    CHECK(sigprocmask(SIG_BLOCK, &set, NULL) == 0);
    s_count = 0;
    (void)raise(SIGUSR1);
    sigemptyset(&cur);
    errno = 0;
    CHECK(sigsuspend(&cur) == -1 && errno == EINTR && s_count == 1);
    CHECK(sigprocmask(SIG_SETMASK, NULL, &cur) == 0 && sigismember(&cur, SIGUSR1) == 1);
    sigemptyset(&set);
    CHECK(sigprocmask(SIG_SETMASK, &set, NULL) == 0);

    /* 무시: 보내도 아무 일 없고, 무시로 바꾸면 대기 중인 것도 버린다 */
    CHECK(signal(SIGUSR2, SIG_IGN) == SIG_DFL && raise(SIGUSR2) == 0);
    CHECK(signal(SIGUSR2, SIG_DFL) == SIG_IGN);

    /* CPU 예외를 가로챈다: 처리기에서 longjmp 로 빠져나간다 (SA_NODEFER: 막은 집합이 그대로 남지 않게) */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    CHECK(sigaction(SIGSEGV, &sa, NULL) == 0 && sigaction(SIGFPE, &sa, NULL) == 0);
    s_signo = 0;
    if (setjmp(s_jb) == 0) {
        int *volatile bad = (int *)16;   /* 포인터를 volatile 로: 컴파일러가 잘못된 접근을 미리 알아채지 않게 */

        *bad = 1;
    }
    CHECK(s_signo == SIGSEGV && s_addr == (void *)16);
    s_signo = 0;
    if (setjmp(s_jb) == 0) {
        volatile int z = 0;
        volatile int q = 7 / z;
        (void)q;
    }
    CHECK(s_signo == SIGFPE);
    (void)signal(SIGSEGV, SIG_DFL);
    (void)signal(SIGFPE, SIG_DFL);

    /* 자식에게: 처리기는 fork 로 물려받는다. 사용자 코드가 도는 중에(비동기) 온 신호도 처리하고 원래 계산을 잇는다 */
    CHECK(set_handler(SIGUSR1, on_count, 0) == 0);
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        volatile double a = 0.0;
        long n = 0;

        s_count = 0;
        while (s_count < 3 && n < 400000000L) {
            a = (double)n * 0.5;
            if (a * 2.0 != (double)n)
                _exit(3);               /* 신호 처리 뒤 FPU 상태가 깨졌다 */
            n++;
        }
        _exit(s_count >= 3 ? 0 : 4);
    }
    CHECK(pid > 0);
    for (i = 0; i < 200; i++) {
        if (waitpid(pid, &st, WNOHANG) == pid)
            break;
        (void)kill(pid, SIGUSR1);
        msleep(20);
    }
    if (i == 200)
        (void)waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* 잠든 read 를 신호가 깨운다: -1 + EINTR */
    CHECK(pipe(p) == 0);
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        ssize_t r;

        close(p[1]);
        r = read(p[0], &ch, 1);         /* 아무도 쓰지 않는다 */
        _exit((r == -1 && errno == EINTR) ? 0 : 5);
    }
    close(p[0]);
    for (i = 0; i < 200; i++) {
        msleep(20);
        if (waitpid(pid, &st, WNOHANG) == pid)
            break;
        (void)kill(pid, SIGUSR1);
    }
    if (i == 200)
        (void)waitpid(pid, &st, 0);
    close(p[1]);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* 기본 동작은 종료: 부모에게는 그 신호로 끝난 것으로 보인다 */
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        (void)signal(SIGUSR1, SIG_DFL);
        for (;;)
            (void)pause();
    }
    msleep(50);
    CHECK(kill(pid, SIGTERM) == 0 && waitpid(pid, &st, 0) == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);

    /* SIGCHLD: 처리기를 둔 부모에게 자식의 종료를 알린다 */
    s_count = 0;
    CHECK(set_handler(SIGCHLD, on_count, 0) == 0);
    fflush(stdout);
    pid = fork();
    if (pid == 0)
        _exit(0);
    while (waitpid(pid, &st, 0) == -1 && errno == EINTR)
        ;
    for (i = 0; i < 50 && s_count == 0; i++)
        msleep(10);
    CHECK(s_count >= 1);
    (void)signal(SIGCHLD, SIG_DFL);
    (void)signal(SIGUSR1, SIG_DFL);
}

static void test_signal(void)
{
    CHECK(signal(SIGINT, on_sig) == SIG_DFL);
    CHECK(raise(SIGINT) == 0 && got_sig == SIGINT);
    CHECK(signal(SIGINT, SIG_IGN) == SIG_DFL);      /* 처리기는 부르기 전에 SIG_DFL 로 돌아갔다 */
    got_sig = 0;
    CHECK(raise(SIGINT) == 0 && got_sig == 0);
    CHECK(signal(0, on_sig) == SIG_ERR && signal(9999, on_sig) == SIG_ERR);
    (void)signal(SIGINT, SIG_DFL);
}

static void test_locale(void)
{
    struct lconv *lc = localeconv();

    CHECK(strcmp(setlocale(LC_ALL, NULL), "C") == 0 && strcmp(setlocale(LC_ALL, ""), "C") == 0);
    CHECK(setlocale(LC_NUMERIC, "ko_KR.UTF-8") == NULL && setlocale(99, "C") == NULL);
    CHECK(lc && strcmp(lc->decimal_point, ".") == 0 && lc->frac_digits == CHAR_MAX);
}

static jmp_buf jb_outer;
static jmp_buf jb_inner;

static void jump_deep(int depth, int val)
{
    volatile char pad[64];          /* 프레임마다 스택을 써서 rsp 복원이 실제로 의미 있게 */

    pad[0] = (char)depth;
    if (depth < 0)
        return;                     /* 오지 않는다: 정상 반환 경로가 없으면 gcc 가 무한 재귀로 본다 */
    if (depth == 0)
        longjmp(jb_outer, val);
    jump_deep(depth - 1, val + pad[0] - depth);
}

/* longjmp 로 빠져나간 뒤에도 이 함수의 callee-saved 레지스터 값(a..f)이 그대로여야 한다 */
static long saved_regs_survive(long seed)
{
    long a = seed * 3;
    long b = seed ^ 0x5555;
    long c = seed + 77;
    long d = seed * seed;
    long e = seed - 1000;
    long f = ~seed;
    int r = setjmp(jb_outer);

    if (r == 0)
        jump_deep(100, 9);
    return (r == 9) ? a + b + c + d + e + f : -1;
}

static void test_setjmp(void)
{
    volatile int count = 0;
    volatile int inner_hit = 0;
    int r;
    long seed = 12345;

    r = setjmp(jb_outer);
    if (r == 0)
        longjmp(jb_outer, 5);
    CHECK(r == 5);

    r = setjmp(jb_outer);
    if (r == 0)
        longjmp(jb_outer, 0);           /* 0 을 넘기면 1 */
    CHECK(r == 1);

    r = setjmp(jb_outer);               /* 같은 jmp_buf 로 여러 번 돌아오기 */
    if (count < 10) {
        count++;
        longjmp(jb_outer, count);
    }
    CHECK(count == 10 && r == 10);

    if (setjmp(jb_outer) == 0) {        /* 중첩: 안쪽 jmp_buf 를 거쳐 바깥으로 */
        if (setjmp(jb_inner) == 0)
            longjmp(jb_inner, 1);
        inner_hit = 1;
        longjmp(jb_outer, 2);
    }
    CHECK(inner_hit == 1);

    CHECK(saved_regs_survive(seed) ==
          seed * 3 + (seed ^ 0x5555) + seed + 77 + seed * seed + seed - 1000 + ~seed);
    CHECK(saved_regs_survive(-7) != -1);

    {   /* 점프 뒤에도 스택이 멀쩡해서 보통의 호출이 동작해야 한다 */
        char buf[32];

        sprintf(buf, "%d-%s", 42, "ok");
        CHECK(strcmp(buf, "42-ok") == 0);
    }
}

/* 환경의 첫 항목 "이름=값" 을 getenv 로 다시 찾아본다 (환경이 비어 있으면 건너뛴다) */
static void test_tmpfile(void)
{
    char n1[L_tmpnam];
    char n2[L_tmpnam];
    char buf[32];
    FILE *f;
    FILE *g;

    CHECK(tmpnam(n1) == n1 && tmpnam(n2) == n2 && strcmp(n1, n2) != 0);
    CHECK(strlen(n1) < L_tmpnam && tmpnam(NULL) != NULL);
    f = tmpfile();
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(fputs("temporary", f) == 0);
    rewind(f);
    CHECK(fgets(buf, sizeof(buf), f) && strcmp(buf, "temporary") == 0);
    CHECK(fclose(f) == 0);
    g = tmpfile();                           /* exit 때 지워져야 한다 (닫지 않는다) */
    CHECK(g != NULL && fputs("left open", g) == 0);
}

static void test_env(char **envp)
{
    CHECK(envp != NULL);
    if (envp && envp[0] && strchr(envp[0], '=')) {
        char name[128];
        size_t n = (size_t)(strchr(envp[0], '=') - envp[0]);

        if (n > 0 && n < sizeof(name)) {
            memcpy(name, envp[0], n);
            name[n] = '\0';
            CHECK(getenv(name) == envp[0] + n + 1);
            name[n - 1] = '\0';              /* 접두어만으로는 찾지 못해야 한다 */
            CHECK(getenv(name) != envp[0] + n && getenv(name) != envp[0] + n + 1);
        }
    }
    CHECK(getenv("NLIBC_NO_SUCH_VARIABLE") == NULL && getenv("") == NULL && getenv("A=B") == NULL);
}

/* ---------------- POSIX ---------------- */
static char self_path[256];             /* 자기 자신 (자식으로 다시 띄운다) */

/* nlibctest --child <일> ... : 부모가 검사하려고 띄운 자식 */
static long read_all(int fd, char *buf, size_t cap);

static int child_main(int argc, char **argv)
{
    char cwd[256];

    if (argc >= 4 && strcmp(argv[2], "exit") == 0)
        return atoi(argv[3]);
    if (argc >= 3 && strcmp(argv[2], "env") == 0)
        return (getenv("NL_SPAWN") && strcmp(getenv("NL_SPAWN"), "hello") == 0) ? 0 : 1;
    if (argc >= 4 && strcmp(argv[2], "cwd") == 0)
        return (getcwd(cwd, sizeof(cwd)) && strcmp(cwd, argv[3]) == 0) ? 0 : 1;
    if (argc >= 5 && strcmp(argv[2], "args") == 0)
        return (strcmp(argv[3], "a b") == 0 && strcmp(argv[4], "c\"d") == 0 && argc == 5) ? 0 : 1;
    if (argc >= 4 && strcmp(argv[2], "echo") == 0) {
        (void)write(1, argv[3], strlen(argv[3]));
        return 0;
    }
    if (argc >= 4 && strcmp(argv[2], "fdclosed") == 0)
        return (fcntl(atoi(argv[3]), F_GETFD) == -1 && errno == EBADF) ? 0 : 1;
    if (argc >= 4 && strcmp(argv[2], "fdopen") == 0)
        return fcntl(atoi(argv[3]), F_GETFD) >= 0 ? 0 : 1;
    if (argc >= 4 && strcmp(argv[2], "ppid") == 0)
        return getppid() == (pid_t)atoi(argv[3]) ? 0 : 1;
    if (argc >= 3 && strcmp(argv[2], "upper") == 0) {    /* stdin -> 대문자 -> stdout */
        char b[256];
        ssize_t n;
        ssize_t i;

        while ((n = read(0, b, sizeof(b))) > 0) {
            for (i = 0; i < n; i++)
                b[i] = (char)toupper((unsigned char)b[i]);
            if (write(1, b, (size_t)n) != n)
                return 2;
        }
        return n == 0 ? 0 : 3;
    }
    if (argc >= 4 && strcmp(argv[2], "devstream") == 0) {  /* 표준 스트림 장치: argv[3] = "dev:/" 또는 "/dev/" */
        char name[32];
        char b[64];
        FILE *f;
        int fd;
        long n;

        sprintf(name, "%sstdout", argv[3]);
        f = fopen(name, "w");
        if (!f || fputs("A", f) == EOF || fclose(f) != 0)
            return 1;
        printf("B");
        fflush(stdout);
        sprintf(name, "%sstdin", argv[3]);
        fd = open(name, O_RDONLY);
        if (fd < 0)
            return 2;
        n = read_all(fd, b, sizeof(b));
        close(fd);
        if (n < 0 || write(1, b, (size_t)n) != n)
            return 3;
        close(1);                                       /* 닫힌 번호의 스트림 장치는 없다 */
        sprintf(name, "%sstdout", argv[3]);
        return (open(name, O_WRONLY) == -1 && errno == ENOENT) ? 0 : 4;
    }
    if (argc >= 3 && strcmp(argv[2], "loop") == 0) {
        for (;;)
            sleep(1);
    }
    return 99;
}

static pid_t spawn_child(const char *a1, const char *a2)
{
    char *args[6];
    pid_t pid = -1;
    int err;

    args[0] = self_path;
    args[1] = (char *)"--child";
    args[2] = (char *)a1;
    args[3] = (char *)a2;
    args[4] = NULL;
    err = posix_spawnp(&pid, self_path, NULL, NULL, args, environ);
    if (err != 0) {
        printf("FAIL posix_spawnp(%s): %s\n", self_path, strerror(err));
        return -1;
    }
    return pid;
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int ends_with(const char *s, const char *tail)
{
    size_t a = strlen(s);
    size_t b = strlen(tail);

    return a >= b && strcmp(s + a - b, tail) == 0;
}

static void test_posix_fd(const char *dir)
{
    char path[256];
    char buf[64];
    int fd;
    int a;
    int b;
    int c;
    struct stat st;
    FILE *f;

    sprintf(path, "%snl_posix.txt", dir);
    (void)unlink(path);

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 3);
    CHECK(write(fd, "hello world", 11) == 11 && close(fd) == 0);
    fd = open(path, O_RDONLY);
    CHECK(fd >= 3);
    memset(buf, 0, sizeof(buf));
    CHECK(read(fd, buf, 5) == 5 && memcmp(buf, "hello", 5) == 0);
    CHECK(lseek(fd, 6, SEEK_SET) == 6 && read(fd, buf, 5) == 5 && memcmp(buf, "world", 5) == 0);
    CHECK(lseek(fd, 0, SEEK_END) == 11 && read(fd, buf, 5) == 0);
    CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 11);
    c = dup(fd);
    CHECK(c >= 0 && c != fd && lseek(c, 0, SEEK_SET) == 0 && read(c, buf, 1) == 1 && buf[0] == 'h');
    CHECK(close(c) == 0 && close(fd) == 0);
    CHECK(close(fd) == -1 && errno == EBADF);
    CHECK(read(-1, buf, 1) == -1 && errno == EBADF && write(99, "x", 1) == -1 && errno == EBADF);

    fd = open(path, O_WRONLY | O_APPEND);                    /* 덧붙이기 */
    CHECK(fd >= 0 && lseek(fd, 0, SEEK_SET) == 0 && write(fd, "!", 1) == 1);
    CHECK((fcntl(fd, F_GETFL) & O_APPEND) && (fcntl(fd, F_GETFL) & O_ACCMODE) == O_WRONLY);
    close(fd);
    CHECK(stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 12);

    CHECK(open(path, O_WRONLY | O_CREAT | O_EXCL, 0644) == -1 && errno == EEXIST);
    sprintf(buf, "%snl_no_such_file", dir);
    CHECK(open(buf, O_RDONLY) == -1 && errno == ENOENT);
    CHECK(stat(buf, &st) == -1 && errno == ENOENT);

    a = open(path, O_RDONLY);                                /* 가장 낮은 빈 fd */
    b = open(path, O_RDONLY);
    CHECK(a >= 0 && b > a && close(a) == 0);
    c = open(path, O_RDONLY);
    CHECK(c == a);
    close(b);

    f = fdopen(c, "r");                                      /* fd 위의 스트림 */
    CHECK(f != NULL && fileno(f) == c && fgets(buf, sizeof(buf), f) && strcmp(buf, "hello world!") == 0);
    if (f)
        fclose(f);
    CHECK(close(c) == -1 && errno == EBADF);                 /* fclose 가 fd 도 닫았다 */
    CHECK(fileno(stdin) == 0 && fileno(stdout) == 1 && fileno(stderr) == 2);
    CHECK(isatty(c) == 0);
    CHECK(access(path, R_OK | W_OK) == 0 && access(buf, F_OK) == -1 && errno == ENOENT);
    CHECK(unlink(path) == 0 && access(path, F_OK) == -1);
}

static void test_posix_dir(const char *dir)
{
    char base[256];
    char cwd[256];
    char start[256];
    char want[300];
    DIR *d;
    struct dirent *e;
    int n;
    int seen;
    struct stat st;
    pid_t pid;
    int status;

    CHECK(getcwd(start, sizeof(start)) != NULL);
    sprintf(base, "%snl_posix_dir", dir);
    (void)rmdir(base);
    CHECK(mkdir(base, 0755) == 0);
    CHECK(mkdir(base, 0755) == -1 && errno == EEXIST);
    /* Nyxis: 자식이 없는 노드는 파일로 보인다 (노드 = 오브젝트, 종류는 "자식이 있는지") */
    CHECK(stat(base, &st) == 0 && (on_nyxis() ? !S_ISDIR(st.st_mode) : S_ISDIR(st.st_mode)));
    CHECK(chdir(base) == 0 && getcwd(cwd, sizeof(cwd)) && ends_with(cwd, "nl_posix_dir"));
    CHECK(getcwd(cwd, 3) == NULL && errno == ERANGE);

    /* 상대 경로 */
    write_file("a.txt", "A");
    write_file("b.txt", "B");
    CHECK(mkdir("sub", 0755) == 0);
    write_file("sub/c.txt", "C");
    {
        FILE *f = fopen("sub/../a.txt", "r");                /* ".." 정리 */

        CHECK(f != NULL && fgetc(f) == 'A');
        if (f)
            fclose(f);
    }

    d = opendir(".");
    CHECK(d != NULL);
    if (d) {
        for (n = 0, seen = 0; (e = readdir(d)) != NULL; n++) {
            if (strcmp(e->d_name, "a.txt") == 0 && e->d_type == DT_REG) seen |= 1;
            if (strcmp(e->d_name, "b.txt") == 0 && e->d_type == DT_REG) seen |= 2;
            if (strcmp(e->d_name, "sub") == 0 && e->d_type == DT_DIR) seen |= 4;
        }
        CHECK(n == 3 && seen == 7);
        rewinddir(d);
        for (n = 0; readdir(d) != NULL; n++)
            ;
        CHECK(n == 3 && closedir(d) == 0);
    }
    if (on_nyxis()) {
        /* Nyxis: 노드는 파일/디렉터리 구분이 없는 오브젝트 (ABI 2.5). 자식이 없는 노드도 목록(빈)과 cd 가 되고,
         * 자식이 있는 노드에도 내용을 읽고 쓴다. 종류(S_ISDIR)는 "지금 자식이 있는지" */
        char got[8];
        FILE *f;

        d = opendir("a.txt");
        CHECK(d != NULL && readdir(d) == NULL);
        if (d)
            closedir(d);
        CHECK(chdir("a.txt") == 0 && chdir("..") == 0);
        f = fopen("sub", "w");                                   /* 자식(c.txt)이 있는 노드의 내용 */
        CHECK(f != NULL && fputs("SUB", f) >= 0 && fclose(f) == 0);
        f = fopen("sub", "r");
        CHECK(f != NULL && fgets(got, sizeof(got), f) != NULL && strcmp(got, "SUB") == 0);
        if (f)
            fclose(f);
        f = fopen("sub/c.txt", "r");                             /* 자식은 그대로 */
        CHECK(f != NULL && fgetc(f) == 'C');
        if (f)
            fclose(f);
        f = fopen("sub", "w");                                   /* 자르기(O_TRUNC)도 자식을 지우지 않는다 */
        CHECK(f != NULL && fclose(f) == 0 && stat("sub", &st) == 0 && st.st_size == 0 && S_ISDIR(st.st_mode));
        CHECK(stat("sub/c.txt", &st) == 0);
        CHECK(stat("a.txt", &st) == 0 && !S_ISDIR(st.st_mode));  /* 자식이 생기면 디렉터리로 보인다 */
        /* 자식을 만들고 찾으려면 그 노드에 통과 권한(x)이 있어야 한다 (UNIX 와 같은 보안 규칙) */
        CHECK(chmod("a.txt", 0755) == 0);
        write_file("a.txt/kid", "K");
        CHECK(stat("a.txt", &st) == 0 && S_ISDIR(st.st_mode) && st.st_size == 1);
        CHECK(unlink("a.txt") == -1 && errno == EISDIR);         /* 자식이 있으면 unlink 하지 않는다 */
        CHECK(unlink("a.txt/kid") == 0 && stat("a.txt", &st) == 0 && !S_ISDIR(st.st_mode));
    } else {
        CHECK(opendir("a.txt") == NULL && errno == ENOTDIR);
        CHECK(rmdir("a.txt") == -1 && errno == ENOTDIR);
        CHECK(chdir("a.txt") == -1 && errno == ENOTDIR);
    }
    CHECK(opendir("nope") == NULL && errno == ENOENT);
    CHECK(unlink("sub") == -1 && errno == EISDIR);
    CHECK(rmdir("sub") == -1 && errno == ENOTEMPTY);
    CHECK(chdir("nope") == -1 && errno == ENOENT);

    CHECK(chdir("sub") == 0 && getcwd(cwd, sizeof(cwd)) && ends_with(cwd, "nl_posix_dir/sub"));
    /* 자식은 현재 디렉터리를 물려받는다 */
    pid = spawn_child("cwd", cwd);
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(chdir("..") == 0 && getcwd(cwd, sizeof(cwd)) && ends_with(cwd, "nl_posix_dir"));
    CHECK(chdir("../nl_posix_dir/./sub/..") == 0 && getcwd(want, sizeof(want)) && strcmp(want, cwd) == 0);

    CHECK(unlink("sub/c.txt") == 0 && rmdir("sub") == 0 && unlink("a.txt") == 0 && unlink("b.txt") == 0);
    CHECK(chdir(start) == 0 && rmdir(base) == 0 && stat(base, &st) == -1);
}

/* getline/getdelim, termios/ioctl 의 터미널이 아닌 fd */
static void test_getline_termios(void)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    FILE *f = tmpfile();
    struct termios t;
    struct winsize ws;
    int i;
    int p[2];

    CHECK(f != NULL);
    if (!f)
        return;
    fputs("first\n\nlast-no-newline", f);
    for (i = 0; i < 300; i++)               /* 처음 버퍼(128)보다 긴 줄 */
        fputc('x', f);
    fputs("\nA:B:C", f);
    rewind(f);
    n = getline(&line, &cap, f);
    CHECK(n == 6 && strcmp(line, "first\n") == 0 && cap >= 7);
    n = getline(&line, &cap, f);
    CHECK(n == 1 && strcmp(line, "\n") == 0);
    n = getline(&line, &cap, f);
    CHECK(n == 15 + 300 + 1 && line[n - 1] == '\n' && line[n] == '\0' && cap > (size_t)n);
    n = getdelim(&line, &cap, ':', f);
    CHECK(n == 2 && strcmp(line, "A:") == 0);
    n = getdelim(&line, &cap, ':', f);
    CHECK(n == 2 && strcmp(line, "B:") == 0);
    n = getline(&line, &cap, f);
    CHECK(n == 1 && strcmp(line, "C") == 0);  /* 끝 줄바꿈 없음 */
    n = getline(&line, &cap, f);
    CHECK(n == -1 && feof(f));
    errno = 0;
    CHECK(getline(NULL, &cap, f) == -1 && errno == EINVAL);
    free(line);
    fclose(f);

    /* 터미널이 아닌 fd: ENOTTY */
    CHECK(pipe(p) == 0);
    errno = 0;
    CHECK(tcgetattr(p[0], &t) == -1 && errno == ENOTTY);
    errno = 0;
    CHECK(tcsetattr(p[0], TCSAFLUSH, &t) == -1 && errno == ENOTTY);
    errno = 0;
    CHECK(ioctl(p[0], TIOCGWINSZ, &ws) == -1 && errno == ENOTTY);
    CHECK(isatty(p[0]) == 0 && errno == ENOTTY);
    close(p[0]);
    close(p[1]);
    errno = 0;
    CHECK(tcgetattr(-1, &t) == -1 && errno == EBADF);
    CHECK(strcmp(strerror(ENOTTY), "Not a terminal") == 0);

    /* cfmakeraw */
    memset(&t, 0, sizeof(t));
    t.c_lflag = ICANON | ECHO | ISIG;
    t.c_iflag = ICRNL | IXON;
    t.c_oflag = OPOST;
    cfmakeraw(&t);
    CHECK(!(t.c_lflag & (ICANON | ECHO | ISIG)) && !(t.c_iflag & (ICRNL | IXON)) && !(t.c_oflag & OPOST));
    CHECK((t.c_cflag & CSIZE) == CS8 && t.c_cc[VMIN] == 1 && t.c_cc[VTIME] == 0);
    CHECK(SIGWINCH > 0 && signal(SIGWINCH, SIG_IGN) != SIG_ERR);
}

static void test_posix_proc(void)
{
    pid_t pid;
    pid_t p2;
    int status = 0;
    int s2 = 0;
    int got;
    char cmd[400];
    posix_spawn_file_actions_t fa;
    struct timespec t0;
    struct timespec t1;
    struct timespec nap;

    pid = spawn_child("exit", "7");
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WIFSIGNALED(status)
          && WEXITSTATUS(status) == 7);
    CHECK(waitpid(pid, &status, 0) == -1 && errno == ECHILD);

    CHECK(setenv("NL_SPAWN", "hello", 1) == 0);              /* 환경을 넘긴다 */
    pid = spawn_child("env", NULL);
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 0);

    pid = spawn_child("exit", "3");                          /* 아무 자식이나 기다리기 */
    p2 = spawn_child("exit", "4");
    got = 0;
    CHECK(pid > 0 && p2 > 0 && pid != p2);
    if (wait(&status) > 0)
        got |= 1 << WEXITSTATUS(status);
    if (wait(&s2) > 0)
        got |= 1 << WEXITSTATUS(s2);
    CHECK(got == ((1 << 3) | (1 << 4)));
    CHECK(wait(&status) == -1 && errno == ECHILD);

    pid = spawn_child("loop", NULL);                         /* 죽이기 */
    CHECK(pid > 0 && waitpid(pid, &status, WNOHANG) == 0 && kill(pid, 0) == 0);
    CHECK(kill(pid, SIGTERM) == 0);
    CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);   /* 진짜 SIGTERM (기본 동작 = 종료) */
    CHECK(kill(getpid() + 100000, SIGTERM) == -1 && errno == ESRCH);
    CHECK(kill(getpid(), 0) == 0 && waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);

    CHECK(system(NULL) != 0);
    sprintf(cmd, "%s --child exit 5", self_path);
    status = system(cmd);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 5);
    sprintf(cmd, "  %s  --child args 'a b' \"c\\\"d\"  ", self_path);
    CHECK(system(cmd) == 0);
    status = system("nl_no_such_command_xyz");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 127);

    CHECK(posix_spawn_file_actions_init(&fa) == 0 && posix_spawn_file_actions_addclose(&fa, -1) == EBADF);
    posix_spawn_file_actions_destroy(&fa);

    CHECK(getpid() > 0 && getuid() == geteuid() && getgid() == getegid());
    CHECK(getlogin() == NULL || strlen(getlogin()) > 0);

    CHECK(clock_gettime(CLOCK_MONOTONIC, &t0) == 0);
    nap.tv_sec = 0;
    nap.tv_nsec = 30000000L;
    CHECK(nanosleep(&nap, NULL) == 0 && sleep(0) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t1) == 0);
    CHECK((t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec) >= 30000000L);
    nap.tv_nsec = 1000000000L;
    CHECK(nanosleep(&nap, NULL) == -1 && errno == EINVAL);
    CHECK(clock_gettime(CLOCK_REALTIME, &t1) == 0 && t1.tv_sec > 1600000000L);
    CHECK(clock_gettime(99, &t1) == -1 && errno == EINVAL);

    CHECK(setenv("NL_X", "1", 1) == 0 && setenv("NL_X", "2", 0) == 0 && strcmp(getenv("NL_X"), "1") == 0);
    CHECK(setenv("NL_X", "3", 1) == 0 && strcmp(getenv("NL_X"), "3") == 0);
    CHECK(unsetenv("NL_X") == 0 && getenv("NL_X") == NULL && getenv("NL_SPAWN") != NULL);
    CHECK(setenv("A=B", "x", 1) == -1 && errno == EINVAL);
}

/* fd 에서 EOF 까지 읽어 buf 에 (NUL 로 끝냄). 반환: 읽은 바이트 수 */
static long read_all(int fd, char *buf, size_t cap)
{
    size_t got = 0;
    ssize_t n;

    while (got + 1 < cap && (n = read(fd, buf + got, cap - 1 - got)) > 0)
        got += (size_t)n;
    buf[got] = '\0';
    return (long)got;
}

static int run_child(char **args)
{
    pid_t pid;
    int status = -1;

    if (posix_spawnp(&pid, self_path, NULL, NULL, args, environ) != 0)
        return -1;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static void test_fork_exec(const char *dir)
{
    char path[256];
    char buf[64];
    int x = 1;
    int fd;
    pid_t pid;
    pid_t me = getpid();
    int status = 0;
    char *args[6];

    /* fork: 메모리는 복사본, 반환값, getppid */
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        x = 2;
        _exit(x == 2 && getppid() == me ? 10 : 11);
    }
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 10);
    CHECK(x == 1);

    /* fork 한 부모와 자식은 열린 파일(오프셋)을 공유한다 */
    sprintf(path, "%snl_fork.txt", dir);
    write_file(path, "abcdef");
    fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    pid = fork();
    if (pid == 0)
        _exit(read(fd, buf, 3) == 3 ? 0 : 1);
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 0);
    memset(buf, 0, sizeof(buf));
    CHECK(read(fd, buf, sizeof(buf) - 1) == 3 && strcmp(buf, "def") == 0);
    close(fd);

    /* fork + exec */
    args[0] = self_path;
    args[1] = (char *)"--child";
    args[2] = (char *)"exit";
    args[3] = (char *)"9";
    args[4] = NULL;
    pid = fork();
    if (pid == 0) {
        execvp(self_path, args);
        _exit(99);
    }
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 9);

    /* exec 실패는 옛 프로그램으로 돌아온다 */
    sprintf(buf, "%snl_no_such.run", dir);
    CHECK(execv(buf, args) == -1 && errno == ENOENT);

    /* 자식의 getppid */
    sprintf(buf, "%d", (int)me);
    args[2] = (char *)"ppid";
    args[3] = buf;
    CHECK(run_child(args) == 0);
    (void)unlink(path);
}

static void test_pipe_dup(const char *dir)
{
    int p[2];
    int q[2];
    int saved;
    int fd;
    int fd2;
    char buf[256];
    char path[256];
    char num[16];
    char *args[6];
    pid_t pid;
    int status = 0;
    long sum;
    int i;

    /* 기본: 쓰고 읽기, EOF, 끊긴 파이프 */
    CHECK(pipe(p) == 0 && p[0] >= 0 && p[1] >= 0 && p[0] != p[1]);
    CHECK(write(p[1], "hello", 5) == 5 && read(p[0], buf, sizeof(buf)) == 5 && memcmp(buf, "hello", 5) == 0);
    CHECK(close(p[1]) == 0 && read(p[0], buf, sizeof(buf)) == 0);
    CHECK(close(p[0]) == 0);
    CHECK(pipe(q) == 0 && close(q[0]) == 0);
    CHECK(write(q[1], "x", 1) == -1 && errno == EPIPE);
    close(q[1]);

    /* poll: 빈 파이프는 시간 초과, 쓰면 POLLIN, 쓰는 쪽을 닫으면 POLLHUP, 닫힌 fd 는 POLLNVAL */
    {
        struct pollfd pf[2];

        CHECK(pipe(p) == 0);
        pf[0].fd = p[0];
        pf[0].events = POLLIN;
        pf[1].fd = -1;                              /* 건너뛴다 */
        pf[1].events = POLLIN;
        CHECK(poll(pf, 2, 0) == 0 && pf[0].revents == 0 && pf[1].revents == 0);
        CHECK(write(p[1], "z", 1) == 1 && poll(pf, 2, 1000) == 1 && (pf[0].revents & POLLIN));
        CHECK(read(p[0], buf, 1) == 1);
        pf[1].fd = p[1];
        pf[1].events = POLLOUT;
        CHECK(poll(pf, 2, 0) == 1 && pf[0].revents == 0 && (pf[1].revents & POLLOUT));
        CHECK(close(p[1]) == 0);
        pf[1].fd = p[1];                            /* 이제 닫힌 fd */
        CHECK(poll(pf, 2, 1000) == 2 && (pf[0].revents & POLLHUP) && pf[1].revents == POLLNVAL);
        CHECK(close(p[0]) == 0);
    }

    /* 버퍼(4KiB)보다 많이: 부모가 쓰고 자식이 읽어 합을 확인 (쓰는 쪽이 공간을 기다린다) */
    CHECK(pipe(p) == 0);
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        long n;
        long total = 0;

        close(p[1]);
        sum = 0;
        while ((n = read(p[0], buf, sizeof(buf))) > 0) {
            for (i = 0; i < n; i++)
                sum += (unsigned char)buf[i];
            total += n;
        }
        _exit(total == 20000 && sum == 20000L * 7 ? 0 : 1);
    }
    close(p[0]);
    memset(buf, 7, sizeof(buf));
    for (i = 0, sum = 0; i < 20000 / 200; i++)
        sum += write(p[1], buf, 200);
    close(p[1]);
    CHECK(sum == 20000 && waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 0);

    /* dup2 로 stdout 을 파이프로: stdio 도 따라간다 */
    fflush(stdout);
    CHECK(pipe(p) == 0);
    saved = dup(1);
    CHECK(saved >= 0 && dup2(p[1], 1) == 1);
    printf("redirected %d\n", 42);
    fflush(stdout);
    CHECK(dup2(saved, 1) == 1 && close(saved) == 0 && close(p[1]) == 0);
    CHECK(read_all(p[0], buf, sizeof(buf)) == 14 && strcmp(buf, "redirected 42\n") == 0);
    close(p[0]);

    /* dup2(fd, fd), F_DUPFD, 공유 오프셋 */
    sprintf(path, "%snl_dup.txt", dir);
    write_file(path, "0123456789");
    fd = open(path, O_RDONLY);
    CHECK(fd >= 0 && dup2(fd, fd) == fd);
    fd2 = fcntl(fd, F_DUPFD, 20);
    CHECK(fd2 >= 20 && read(fd, buf, 4) == 4 && read(fd2, buf, 2) == 2 && memcmp(buf, "45", 2) == 0);
    CHECK(close(fd2) == 0);
    CHECK(dup2(fd, 25) == 25 && lseek(25, 0, SEEK_CUR) == 6 && close(25) == 0);

    /* FD_CLOEXEC: exec/spawn 한 자식에게 넘어가지 않는다 */
    fd2 = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(fd2 >= 0 && fcntl(fd2, F_GETFD) == FD_CLOEXEC && fcntl(fd, F_GETFD) == 0);
    args[0] = self_path;
    args[1] = (char *)"--child";
    args[4] = NULL;
    args[2] = (char *)"fdclosed";
    sprintf(num, "%d", fd2);
    args[3] = num;
    CHECK(run_child(args) == 0);
    args[2] = (char *)"fdopen";
    sprintf(num, "%d", fd);
    args[3] = num;
    CHECK(run_child(args) == 0);
    CHECK(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && fcntl(fd, F_GETFD) == FD_CLOEXEC);
    args[2] = (char *)"fdclosed";
    CHECK(run_child(args) == 0);
    pid = fork();                                    /* fork 는 CLOEXEC 와 상관없이 물려준다 */
    if (pid == 0)
        _exit(fcntl(fd, F_GETFD) == FD_CLOEXEC && fcntl(fd2, F_GETFD) == FD_CLOEXEC ? 0 : 1);
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 0);
    close(fd);
    close(fd2);
    (void)unlink(path);
}

static void test_spawn_actions(void)
{
    posix_spawn_file_actions_t fa;
    int out[2];
    int in[2];
    char buf[128];
    char *args[6];
    pid_t pid;
    int status = 0;
    int err;

    args[0] = self_path;
    args[1] = (char *)"--child";
    args[2] = (char *)"echo";
    args[3] = (char *)"spawned";
    args[4] = NULL;

    /* 자식의 stdout 을 파이프로 */
    CHECK(pipe(out) == 0);
    CHECK(posix_spawn_file_actions_init(&fa) == 0);
    CHECK(posix_spawn_file_actions_adddup2(&fa, out[1], 1) == 0);
    CHECK(posix_spawn_file_actions_addclose(&fa, out[0]) == 0 && posix_spawn_file_actions_addclose(&fa, out[1]) == 0);
    err = posix_spawnp(&pid, self_path, &fa, NULL, args, environ);
    CHECK(err == 0);
    close(out[1]);
    CHECK(read_all(out[0], buf, sizeof(buf)) == 7 && strcmp(buf, "spawned") == 0);
    CHECK(err != 0 || (waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 0));
    close(out[0]);

    posix_spawn_file_actions_destroy(&fa);

    /* 실행 실패도 posix_spawn 의 반환값으로 (fork 경로: 자식이 CLOEXEC 파이프로 errno 를 보낸다) */
    posix_spawn_file_actions_init(&fa);
    CHECK(posix_spawn_file_actions_adddup2(&fa, 1, 1) == 0);
    CHECK(posix_spawn(&pid, "nl_no_such_program.run", &fa, NULL, args, environ) == ENOENT);
    CHECK(wait(&status) == -1 && errno == ECHILD);           /* 실패한 자식은 이미 거두었다 */
    posix_spawn_file_actions_destroy(&fa);

    /* 파이프라인: 부모 -> [upper] -> 부모 */
    CHECK(pipe(in) == 0 && pipe(out) == 0);
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addclose(&fa, in[0]);
    posix_spawn_file_actions_addclose(&fa, in[1]);
    posix_spawn_file_actions_addclose(&fa, out[0]);
    posix_spawn_file_actions_addclose(&fa, out[1]);
    args[2] = (char *)"upper";
    args[3] = NULL;
    err = posix_spawnp(&pid, self_path, &fa, NULL, args, environ);
    posix_spawn_file_actions_destroy(&fa);
    CHECK(err == 0);
    close(in[0]);
    close(out[1]);
    CHECK(write(in[1], "pipe line", 9) == 9 && close(in[1]) == 0);
    CHECK(read_all(out[0], buf, sizeof(buf)) == 9 && strcmp(buf, "PIPE LINE") == 0);
    close(out[0]);
    CHECK(err != 0 || (waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0));
}

static void test_modes(const char *dir)
{
    char path[256];
    char d[256];
    struct stat st;
    mode_t old;
    int fd;
    char *args[3];

    sprintf(path, "%snl_mode.txt", dir);
    sprintf(d, "%snl_mode_dir", dir);
    (void)unlink(path);
    (void)rmdir(d);

    old = umask(027);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && (st.st_mode & 0777) == 0640 && st.st_uid == getuid());
    close(fd);
    CHECK(mkdir(d, 0777) == 0 && stat(d, &st) == 0 && (on_nyxis() || S_ISDIR(st.st_mode)) && (st.st_mode & 0777) == 0750);
    CHECK(umask(old) == 027);

    /* 새로 만든 파일은 모드가 읽기 전용이어도 요청한 접근으로 열린다 (POSIX) */
    CHECK(unlink(path) == 0);
    old = umask(022);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0444);
    CHECK(fd >= 0 && write(fd, "ro", 2) == 2 && close(fd) == 0);
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0444 && st.st_size == 2);
    CHECK((open(path, O_WRONLY) == -1 && errno == EACCES) || getuid() == 0);
    CHECK(unlink(path) == 0);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0466);
    CHECK(fd >= 0 && write(fd, "x", 1) == 1 && close(fd) == 0);
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0444);    /* umask 022 가 적용된다 */
    CHECK(chmod(path, 0644) == 0 && umask(old) == 022);

    CHECK(chmod(path, 0600) == 0 && stat(path, &st) == 0 && (st.st_mode & 0777) == 0600 && S_ISREG(st.st_mode));
    CHECK(chmod(path, 04755) == -1 && errno == EINVAL);
    CHECK(chmod("nl_no_such_file", 0644) == -1 && errno == ENOENT);

    /* 실행 권한: x 비트가 없으면 실행할 수 없다 */
    write_file(path, "not a program");
    CHECK(chmod(path, 0644) == 0 && access(path, X_OK) == -1 && errno == EACCES);
    args[0] = path;
    args[1] = NULL;
    CHECK(execv(path, args) == -1 && errno == EACCES);
    CHECK(chmod(path, 0755) == 0 && access(path, X_OK) == 0);
    CHECK(execv(path, args) == -1 && errno != EACCES);   /* 이제 권한은 있지만 실행 파일 형식이 아니다 */

    CHECK(unlink(path) == 0 && rmdir(d) == 0);
}

/* 표준 스트림 장치 dev:/stdin stdout stderr tty (호스트에서는 /dev/...) */
static void test_std_devices(const char *dir)
{
    int nyx = strchr(dir, ':') != NULL;
    const char *dev = nyx ? "dev:/" : "/dev/";
    posix_spawn_file_actions_t fa;
    int in[2];
    int out[2];
    char buf[64];
    char name[32];
    char path[256];
    char *args[5];
    pid_t pid;
    int status = 0;
    int fd;
    int g;
    int saved;

    /* 자식의 stdin/stdout 을 파이프로: 장치 이름으로 열어도 파이프에 닿는다 */
    args[0] = self_path;
    args[1] = (char *)"--child";
    args[2] = (char *)"devstream";
    args[3] = (char *)dev;
    args[4] = NULL;
    CHECK(pipe(in) == 0 && pipe(out) == 0);
    CHECK(posix_spawn_file_actions_init(&fa) == 0);
    CHECK(posix_spawn_file_actions_adddup2(&fa, in[0], 0) == 0 && posix_spawn_file_actions_adddup2(&fa, out[1], 1) == 0);
    CHECK(posix_spawn_file_actions_addclose(&fa, in[1]) == 0 && posix_spawn_file_actions_addclose(&fa, out[0]) == 0);
    CHECK(posix_spawnp(&pid, self_path, &fa, NULL, args, environ) == 0);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    close(out[1]);
    CHECK(write(in[1], "cd", 2) == 2 && close(in[1]) == 0);
    CHECK(read_all(out[0], buf, sizeof(buf)) == 4 && strcmp(buf, "ABcd") == 0);
    close(out[0]);
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* 이 프로세스의 stdout/stderr 도 장치 이름으로 열린다 */
    sprintf(name, "%sstderr", dev);
    fd = open(name, O_WRONLY);
    CHECK(fd > 2 && write(fd, "", 0) == 0 && close(fd) == 0);

    /* O_APPEND 는 열린 파일의 속성: exec 한 자식이 물려받은 fd 로 써도 끝에 붙는다 (셸의 >>) */
    sprintf(path, "%snl_append.txt", dir);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && write(fd, "ab", 2) == 2 && lseek(fd, 0, SEEK_SET) == 0 && close(fd) == 0);
    fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0 && lseek(fd, 0, SEEK_SET) == 0);
    args[2] = (char *)"echo";
    args[3] = (char *)"cd";
    CHECK(posix_spawn_file_actions_init(&fa) == 0 && posix_spawn_file_actions_adddup2(&fa, fd, 1) == 0);
    CHECK(posix_spawnp(&pid, self_path, &fa, NULL, args, environ) == 0);
    posix_spawn_file_actions_destroy(&fa);
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(fd);
    fd = open(path, O_RDONLY);
    CHECK(fd >= 0 && read_all(fd, buf, sizeof(buf)) == 4 && strcmp(buf, "abcd") == 0);
    close(fd);
    (void)unlink(path);
    if (!nyx)
        return;

    /* Nyxis: 0/1/2 번과 같은 열린 파일을 공유한다 (오프셋까지) */
    sprintf(path, "%snl_devout.txt", dir);
    fflush(stdout);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    saved = dup(1);
    CHECK(fd >= 0 && saved >= 0 && dup2(fd, 1) == 1);
    g = open("dev:/stdout", O_WRONLY);
    CHECK(g >= 0 && write(g, "xy", 2) == 2 && write(1, "z", 1) == 1);
    CHECK(dup2(saved, 1) == 1 && close(saved) == 0 && close(fd) == 0 && close(g) == 0);
    fd = open(path, O_RDONLY);
    CHECK(fd >= 0 && read_all(fd, buf, sizeof(buf)) == 3 && strcmp(buf, "xyz") == 0);
    close(fd);
    (void)unlink(path);

    /* 원래 핸들에 없는 권한은 얻을 수 없다: 콘솔 stdout 은 쓰기 전용 */
    CHECK(open("dev:/stdout", O_RDONLY) == -1 && errno == EACCES);
    CHECK(open("dev:/stdin", O_WRONLY) == -1 && errno == EACCES);

    /* dev:/tty 는 리디렉션과 상관없이 터미널 */
    fd = open("dev:/tty", O_WRONLY);
    CHECK(fd >= 0 && write(fd, "", 0) == 0 && close(fd) == 0);
}

/*
 * 서로 다른 파일을 많이 열고 닫아도 시스템이 계속 동작한다 (커널 VFS 의 경로 캐시가 차면 쓰지 않는 항목을
 * 거둬야 한다. 예전에는 거두지 않아서 64개쯤 뒤로는 어떤 파일도 열 수 없었고 exec 도 실패했다)
 */
static void test_many_files(const char *dir)
{
    char path[256];
    char sub[256];
    int ok = 1;
    int i;
    int fd;
    pid_t pid;
    int status = 0;

    sprintf(sub, "%snl_many", dir);
    (void)mkdir(sub, 0755);
    for (i = 0; i < 300 && ok; i++) {
        sprintf(path, "%s/f%d", sub, i);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || write(fd, "x", 1) != 1 || close(fd) != 0)
            ok = 0;
    }
    CHECK(ok);
    for (i = 0, ok = 1; i < 300 && ok; i++) {          /* 다시 열기: 캐시에서 거둔 항목도 다시 찾는다 */
        char c = 0;

        sprintf(path, "%s/f%d", sub, i);
        fd = open(path, O_RDONLY);
        if (fd < 0 || read(fd, &c, 1) != 1 || c != 'x' || close(fd) != 0)
            ok = 0;
    }
    CHECK(ok);
    fd = open(path, O_RDONLY);                         /* 열어 둔 채로 다른 파일을 많이 연다 */
    for (i = 0; i < 280; i++) {
        int g;

        sprintf(path, "%s/f%d", sub, i);
        g = open(path, O_RDONLY);
        if (g >= 0)
            close(g);
    }
    {
        char c = 0;

        CHECK(fd >= 0 && lseek(fd, 0, SEEK_SET) == 0 && read(fd, &c, 1) == 1 && c == 'x' && close(fd) == 0);
    }
    pid = spawn_child("exit", "0");                    /* exec 도 여전히 된다 */
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 0);
    for (i = 0, ok = 1; i < 300; i++) {
        sprintf(path, "%s/f%d", sub, i);
        if (unlink(path) != 0)
            ok = 0;
    }
    CHECK(ok && rmdir(sub) == 0);
}

static void test_posix(const char *dir, const char *argv0)
{
    /* 자기 경로: 이름만이면(Nyxis: app:/ 에서 찾는다) 그대로, 상대 경로면 지금 디렉터리 기준으로 굳힌다 */
    if (strchr(argv0, '/') && argv0[0] != '/' && strchr(argv0, ':') == NULL) {
        char here[200];

        if (getcwd(here, sizeof(here)))
            sprintf(self_path, "%s/%s", here, argv0);
    } else {
        strcpy(self_path, argv0);
    }
    test_posix_fd(dir);
    test_posix_dir(dir);
    test_posix_proc();
    test_fork_exec(dir);
    test_pipe_dup(dir);
    test_spawn_actions();
    test_modes(dir);
    test_std_devices(dir);
    test_many_files(dir);
}

static void at1(void) { printf(" 1\n"); }
static void at2(void) { printf("atexit 2"); }

int main(int argc, char **argv, char **envp)
{
    const char *dir = argc > 1 ? argv[1] : "tmp:/";

    if (argc > 1 && strcmp(argv[1], "--child") == 0)
        return child_main(argc, argv);

    assert(argc >= 1);
    test_string();
    test_stdbool_stdint();
    test_ctype();
    test_strtol();
    test_printf();
    test_malloc();
    test_sort();
    test_setjmp();
    test_scanf(dir);
    test_float_printf();
    test_strtod();
    test_math();
    test_time();
    test_signal();
    test_locale();
    test_stdio(dir);
    test_env(envp);
    test_tmpfile();
    test_getline_termios();
    test_posix(dir, argv[0]);
    test_sigaction();

    printf("nlibc: %d/%d checks passed\n", checks - failures, checks);
    CHECK(atexit(at1) == 0 && atexit(at2) == 0);
    return failures;
}
