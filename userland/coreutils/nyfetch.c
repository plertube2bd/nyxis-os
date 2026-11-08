/*
 * nyfetch - 시스템 정보 요약 (로고 없음)
 *
 * 값의 출처:
 *   사용자     getlogin()           호스트      info:/hostname
 *   OS         info:/version        ABI         info:/abi
 *   업타임     info:/uptime_ms      메모리      info:/memory/{total,free}_kib
 *   셸         $SHELL (경로의 마지막 이름, ".run" 은 뗀다)
 *   터미널     $TERM                CPU         CPUID 상표 문자열 (사용자 모드에서 실행 가능)
 * 얻지 못한 값은 그 줄을 빼고, 터미널이면(cu_color_ok) 항목 이름에 색을 입히고 끝에 색 견본을 찍는다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "common.h"

static int g_color;

/* path 의 첫 줄을 out 에 (줄바꿈 제외). 실패하면 -1 */
static int read_line(const char *path, char *out, size_t cap)
{
    ssize_t n;
    size_t len = 0;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return -1;
    while (len + 1 < cap && (n = read(fd, out + len, cap - 1 - len)) > 0)
        len += (size_t)n;
    close(fd);
    out[len] = '\0';
    out[strcspn(out, "\n")] = '\0';
    return out[0] ? 0 : -1;
}

static int read_u64(const char *path, unsigned long *v)
{
    char buf[32];
    char *end;

    if (read_line(path, buf, sizeof(buf)) < 0)
        return -1;
    *v = strtoul(buf, &end, 10);
    return (*end == '\0') ? 0 : -1;
}

static void field(const char *key, const char *val)
{
    if (!val || !*val)
        return;
    if (g_color)
        printf("\033[1;36m%s\033[0m: %s\n", key, val);
    else
        printf("%s: %s\n", key, val);
}

static void fmt_uptime(unsigned long ms, char *out, size_t cap)
{
    unsigned long s = ms / 1000UL;
    unsigned long d = s / 86400UL, h = (s / 3600UL) % 24UL, m = (s / 60UL) % 60UL;
    size_t len = 0;

    out[0] = '\0';
    if (d)
        len += (size_t)snprintf(out + len, cap - len, "%lu day%s, ", d, d == 1 ? "" : "s");
    if (h && len < cap)
        len += (size_t)snprintf(out + len, cap - len, "%lu hour%s, ", h, h == 1 ? "" : "s");
    if (len < cap) {
        if (d || h || m)
            snprintf(out + len, cap - len, "%lu min%s", m, m == 1 ? "" : "s");
        else
            snprintf(out + len, cap - len, "%lu sec%s", s, s == 1 ? "" : "s");
    }
}

#if defined(__x86_64__) || defined(__i386__)
static void cpuid(unsigned int leaf, unsigned int r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

/* CPUID 0x80000002..4 상표 문자열 (앞뒤 공백과 겹친 공백 정리). 없으면 -1 */
static int cpu_brand(char *out, size_t cap)
{
    unsigned int r[4];
    char raw[49];
    size_t i, j = 0;
    int space = 1;

    cpuid(0x80000000U, r);
    if (r[0] < 0x80000004U)
        return -1;
    for (i = 0; i < 3; i++) {
        cpuid(0x80000002U + (unsigned int)i, r);
        memcpy(raw + i * 16, r, 16);
    }
    raw[48] = '\0';
    for (i = 0; raw[i] && j + 1 < cap; i++) {
        if (raw[i] == ' ') {
            if (space)
                continue;
            space = 1;
        } else {
            space = 0;
        }
        out[j++] = raw[i];
    }
    while (j > 0 && out[j - 1] == ' ')
        j--;
    out[j] = '\0';
    return j ? 0 : -1;
}
#else
static int cpu_brand(char *out, size_t cap)
{
    (void)out;
    (void)cap;
    return -1;
}
#endif

int main(int argc, char **argv)
{
    char host[72], ver[32], abi[32], buf[128], title[160];
    const char *user, *env;
    unsigned long v, total, freek;
    size_t i, tlen;

    (void)argv;
    cu_prog = "nyfetch";
    if (argc > 1) {
        fputs("usage: nyfetch\n", stderr);
        return 2;
    }
    g_color = cu_color_ok(1);

    user = getlogin();
    if (read_line("info:/hostname", host, sizeof(host)) < 0)
        strcpy(host, "nyxis");
    snprintf(title, sizeof(title), "%s@%s", user ? user : "?", host);
    tlen = strlen(title);
    if (g_color)
        printf("\033[1;36m%s\033[0m@\033[1;36m%s\033[0m\n", user ? user : "?", host);
    else
        printf("%s\n", title);
    for (i = 0; i < tlen; i++)
        putchar('-');
    putchar('\n');

    if (read_line("info:/version", ver, sizeof(ver)) == 0) {
        snprintf(buf, sizeof(buf), "Nyxis %s x86_64", ver);
        field("OS", buf);
    }
    if (read_line("info:/abi", abi, sizeof(abi)) == 0)
        field("ABI", abi);
    if (read_u64("info:/uptime_ms", &v) == 0) {
        fmt_uptime(v, buf, sizeof(buf));
        field("Uptime", buf);
    }
    env = getenv("SHELL");
    if (env && *env) {
        size_t n;

        cu_basename(env, buf, sizeof(buf));
        n = strlen(buf);
        if (n > 4 && strcmp(buf + n - 4, ".run") == 0)
            buf[n - 4] = '\0';
        field("Shell", buf);
    }
    field("Terminal", getenv("TERM"));
    if (cpu_brand(buf, sizeof(buf)) == 0)
        field("CPU", buf);
    if (read_u64("info:/memory/total_kib", &total) == 0 &&
        read_u64("info:/memory/free_kib", &freek) == 0 && freek <= total) {
        unsigned long used = total - freek;

        /* 1 MiB 보다 적게 쓰면 MiB 로는 0 이 되므로 KiB 로 */
        if (used < 1024UL)
            snprintf(buf, sizeof(buf), "%lu KiB / %lu MiB (%lu%%)",
                     used, total / 1024UL, total ? used * 100UL / total : 0UL);
        else
            snprintf(buf, sizeof(buf), "%lu MiB / %lu MiB (%lu%%)",
                     used / 1024UL, total / 1024UL, total ? used * 100UL / total : 0UL);
        field("Memory", buf);
    }

    if (g_color) {
        putchar('\n');
        for (i = 0; i < 8; i++)
            printf("\033[4%dm   ", (int)i);
        fputs("\033[0m\n", stdout);
        for (i = 0; i < 8; i++)
            printf("\033[10%dm   ", (int)i);
        fputs("\033[0m\n", stdout);
    }
    return (fflush(stdout) == 0) ? 0 : 1;
}
