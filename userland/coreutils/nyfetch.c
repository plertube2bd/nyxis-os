/*
 * nyfetch - 시스템 정보 요약 (로고 없음)
 *
 * 값의 출처 (Nyxis 고유 값은 nx64api <nx64.h>):
 *   사용자     nx64_whoami          호스트      nx64_info("hostname")
 *   OS         nx64_info("version") ABI         nx64_info("abi")
 *   업타임     nx64_uptime_ms       메모리      nx64_memory: 쓰는 중 = RAM - 남은 것 / RAM
 *                                   (커널/initrd/tmp:/ 처럼 할당기 밖의 RAM 도 사용으로 센다)
 *                                   할당기     옛 Nyxis 방식: (관리 - 남은 것) / 할당기가 관리하는 RAM
 *   셸         $SHELL (경로의 마지막 이름, ".run" 은 뗀다)
 *   터미널     $TERM                CPU         CPUID 상표 문자열 (사용자 모드에서 실행 가능)
 * 얻지 못한 값은 그 줄을 빼고, 터미널이면(cu_color_ok) 항목 이름에 색을 입히고 끝에 색 견본을 찍는다.
 *
 * 설정 fields (nx64_setting_get: 사용자 값 roi:/<uid>/app/nyfetch/fields, 기본값 app:/nyfetch.run/fields):
 *   보일 항목과 순서. 공백으로 나눈 이름: os abi uptime shell terminal cpu memory allocator palette (모르는 이름은 무시).
 *   설정이 없으면 모두 이 순서로.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nx64.h>
#include "common.h"

static int g_color;

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

#define DEFAULT_FIELDS "os abi uptime shell terminal cpu memory allocator palette"

/* "쓴 것 / 전체 (퍼센트)". 1 MiB 보다 적게 쓰면 MiB 로는 0 이 되므로 KiB 로 */
static void fmt_usage(unsigned long used, unsigned long total, char *buf, size_t cap)
{
    unsigned long pct = total ? used * 100UL / total : 0UL;

    if (used < 1024UL)
        snprintf(buf, cap, "%lu KiB / %lu MiB (%lu%%)", used, total / 1024UL, pct);
    else
        snprintf(buf, cap, "%lu MiB / %lu MiB (%lu%%)", used / 1024UL, total / 1024UL, pct);
}

/* 항목 하나를 보인다 (설정 fields 의 이름). 값을 얻지 못하거나 모르는 이름이면 아무것도 안 한다 */
static void show(const char *name)
{
    char ver[32], abi[32], buf[128];
    const char *env;
    struct nx64_memory m;
    unsigned long v;
    size_t i;

    if (strcmp(name, "os") == 0) {
        if (nx64_info("version", ver, sizeof(ver)) > 0) {
            snprintf(buf, sizeof(buf), "Nyxis %s x86_64", ver);
            field("OS", buf);
        }
    } else if (strcmp(name, "abi") == 0) {
        if (nx64_info("abi", abi, sizeof(abi)) > 0)
            field("ABI", abi);
    } else if (strcmp(name, "uptime") == 0) {
        if (nx64_info_ulong("uptime_ms", &v) == 0) {
            fmt_uptime(v, buf, sizeof(buf));
            field("Uptime", buf);
        }
    } else if (strcmp(name, "shell") == 0) {
        env = getenv("SHELL");
        if (env && *env) {
            size_t n;

            cu_basename(env, buf, sizeof(buf));
            n = strlen(buf);
            if (n > 4 && strcmp(buf + n - 4, ".run") == 0)
                buf[n - 4] = '\0';
            field("Shell", buf);
        }
    } else if (strcmp(name, "terminal") == 0) {
        field("Terminal", getenv("TERM"));
    } else if (strcmp(name, "cpu") == 0) {
        if (cpu_brand(buf, sizeof(buf)) == 0)
            field("CPU", buf);
    } else if (strcmp(name, "memory") == 0) {
        /* 지금 방식: 물리 RAM 전체 기준 (할당기 밖의 커널/initrd/tmp:/ 도 사용) */
        if (nx64_memory(&m) == 0 && m.ram_kib > 0) {
            fmt_usage(m.used_kib, m.ram_kib, buf, sizeof(buf));
            field("Memory", buf);
        }
    } else if (strcmp(name, "allocator") == 0) {
        /* 옛 Nyxis 방식: 할당기(pfa)가 관리하는 영역 기준 (total_kib - free_kib) */
        if (nx64_memory(&m) == 0 && m.managed_kib > 0) {
            fmt_usage(m.alloc_kib, m.managed_kib, buf, sizeof(buf));
            field("Allocator", buf);
        }
    } else if (strcmp(name, "palette") == 0 && g_color) {
        putchar('\n');
        for (i = 0; i < 8; i++)
            printf("\033[4%dm   ", (int)i);
        fputs("\033[0m\n", stdout);
        for (i = 0; i < 8; i++)
            printf("\033[10%dm   ", (int)i);
        fputs("\033[0m\n", stdout);
    }
}

int main(int argc, char **argv)
{
    char host[72], title[160], list[256];
    struct nx64_identity me;
    const char *user = "?";
    char *name;
    size_t i, tlen;

    (void)argv;
    cu_prog = "nyfetch";
    if (argc > 1) {
        fputs("usage: nyfetch\n", stderr);
        return 2;
    }
    g_color = cu_color_ok(1);

    if (nx64_whoami(&me) == 0 && me.name[0] != '\0')
        user = me.name;
    if (nx64_info("hostname", host, sizeof(host)) <= 0)
        strcpy(host, "nyxis");
    snprintf(title, sizeof(title), "%s@%s", user, host);
    tlen = strlen(title);
    if (g_color)
        printf("\033[1;36m%s\033[0m@\033[1;36m%s\033[0m\n", user, host);
    else
        printf("%s\n", title);
    for (i = 0; i < tlen; i++)
        putchar('-');
    putchar('\n');

    if (nx64_setting_get("nyfetch", "fields", list, sizeof(list)) < 0)
        strcpy(list, DEFAULT_FIELDS);
    for (name = strtok(list, " \t\n"); name != NULL; name = strtok(NULL, " \t\n"))
        show(name);
    return (fflush(stdout) == 0) ? 0 : 1;
}
