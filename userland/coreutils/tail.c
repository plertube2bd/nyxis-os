/*
 * tail [-n 줄수] [파일...] - 끝부분 줄을 출력한다 (기본 10줄, 파일이 없거나 "-" 이면 표준 입력)
 * 입력 전체를 메모리에 읽은 뒤 끝에서부터 센다.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "common.h"

static int tail_one(const char *path, long lines)
{
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    size_t pos;
    ssize_t n;
    int fd = strcmp(path, "-") == 0 ? 0 : open(path, O_RDONLY);

    if (fd < 0) {
        cu_warn(path);
        return 1;
    }
    for (;;) {
        if (len == cap) {
            size_t nc = cap ? cap * 2 : 4096;
            char *nb = (char *)realloc(buf, nc);

            if (!nb) {
                errno = ENOMEM;
                n = -1;
                break;
            }
            buf = nb;
            cap = nc;
        }
        n = read(fd, buf + len, cap - len);
        if (n <= 0)
            break;
        len += (size_t)n;
    }
    if (fd != 0)
        close(fd);
    if (n < 0) {
        cu_warn(path);
        free(buf);
        return 1;
    }

    /* 끝의 줄바꿈은 마지막 줄의 일부다: 그 앞에서부터 거꾸로 '\n' 을 lines 개 만나면 그 뒤가 시작 */
    pos = (len > 0 && buf[len - 1] == '\n') ? len - 1 : len;
    if (lines == 0) {
        pos = len;
    } else {
        long seen = 0;

        while (pos > 0) {
            if (buf[pos - 1] == '\n' && ++seen == lines)
                break;
            pos--;
        }
    }
    fwrite(buf + pos, 1, len - pos, stdout);
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    long lines = 10;
    char *end;
    int rc = 0;
    int c;
    int i;

    cu_prog = "tail";
    while ((c = cu_getopt(argc, argv, "n:")) != -1) {
        if (c == '?')
            return 2;
        lines = strtol(cu_optarg, &end, 10);
        if (*cu_optarg == '\0' || *end != '\0' || lines < 0) {
            cu_msg("invalid number of lines '%s'", cu_optarg);
            return 2;
        }
    }
    if (cu_optind >= argc)
        return tail_one("-", lines);
    for (i = cu_optind; i < argc; i++) {
        if (argc - cu_optind > 1)
            printf("%s==> %s <==\n", i > cu_optind ? "\n" : "", argv[i]);
        fflush(stdout);
        rc |= tail_one(argv[i], lines);
    }
    return rc;
}
