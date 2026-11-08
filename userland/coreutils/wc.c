/*
 * wc [-lwc] [파일...] - 줄, 단어, 바이트 수 (기본은 셋 다. 파일이 없거나 "-" 이면 표준 입력)
 * 파일이 둘 이상이면 마지막에 합계(total).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "common.h"

static int g_l = 0, g_w = 0, g_c = 0;
static long t_l = 0, t_w = 0, t_c = 0;

static void report(long l, long w, long c, const char *name)
{
    if (g_l)
        printf("%7ld", l);
    if (g_w)
        printf("%s%7ld", g_l ? " " : "", w);
    if (g_c)
        printf("%s%7ld", (g_l || g_w) ? " " : "", c);
    if (name)
        printf(" %s", name);
    putchar('\n');
}

static int wc_one(const char *path, int show_name)
{
    char buf[4096];
    long l = 0, w = 0, c = 0;
    int in_word = 0;
    ssize_t n;
    ssize_t i;
    int fd = strcmp(path, "-") == 0 ? 0 : open(path, O_RDONLY);

    if (fd < 0) {
        cu_warn(path);
        return 1;
    }
    while ((n = read(fd, buf, sizeof(buf))) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            cu_warn(path);
            if (fd != 0)
                close(fd);
            return 1;
        }
        c += n;
        for (i = 0; i < n; i++) {
            char ch = buf[i];
            int space = ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r' || ch == '\v' || ch == '\f';

            if (ch == '\n')
                l++;
            if (!space && !in_word)
                w++;
            in_word = !space;
        }
    }
    if (fd != 0)
        close(fd);
    report(l, w, c, show_name ? path : NULL);
    t_l += l;
    t_w += w;
    t_c += c;
    return 0;
}

int main(int argc, char **argv)
{
    int rc = 0;
    int c;
    int i;

    cu_prog = "wc";
    while ((c = cu_getopt(argc, argv, "lwc")) != -1) {
        if (c == '?')
            return 2;
        if (c == 'l')
            g_l = 1;
        else if (c == 'w')
            g_w = 1;
        else
            g_c = 1;
    }
    if (!g_l && !g_w && !g_c)
        g_l = g_w = g_c = 1;
    if (cu_optind >= argc)
        return wc_one("-", 0);
    for (i = cu_optind; i < argc; i++)
        rc |= wc_one(argv[i], 1);
    if (argc - cu_optind > 1)
        report(t_l, t_w, t_c, "total");
    return rc;
}
