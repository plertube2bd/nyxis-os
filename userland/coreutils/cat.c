/*
 * cat [-n] [파일...] - 파일을 이어서 출력한다. 파일이 없거나 "-" 이면 표준 입력.
 *   -n  줄 번호를 붙인다
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "common.h"

static int g_number = 0;
static long g_line = 1;
static int g_at_start = 1;      /* 줄 번호는 파일이 바뀌어도 이어진다 (GNU cat 과 같다) */

static int number_fd(int fd)
{
    char buf[4096];
    ssize_t n;
    ssize_t i;

    while ((n = read(fd, buf, sizeof(buf))) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        for (i = 0; i < n; i++) {
            if (g_at_start) {
                printf("%6ld\t", g_line++);
                g_at_start = 0;
            }
            putchar(buf[i]);
            if (buf[i] == '\n')
                g_at_start = 1;
        }
    }
    return 0;
}

static int cat_one(const char *path)
{
    int fd = 0;
    int r;

    if (strcmp(path, "-") != 0) {
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            cu_warn(path);
            return 1;
        }
    }
    r = g_number ? number_fd(fd) : cu_copy_fd(fd, 1);
    if (r < 0)
        cu_warn(path);
    if (fd != 0)
        close(fd);
    return r < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    int c;
    int rc = 0;
    int i;

    cu_prog = "cat";
    while ((c = cu_getopt(argc, argv, "n")) != -1) {
        if (c == '?')
            return 2;
        g_number = 1;
    }
    if (cu_optind >= argc)
        rc |= cat_one("-");
    for (i = cu_optind; i < argc; i++)
        rc |= cat_one(argv[i]);
    if (fflush(stdout) != 0)
        rc = 1;
    return rc;
}
