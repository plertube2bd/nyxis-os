/*
 * cmp [-s] 파일1 파일2 - 두 파일을 바이트 단위로 비교한다
 *   같으면 0, 다르면 첫 차이를 "a b differ: byte N, line L" 로 알리고 1, 오류면 2.
 *   -s  아무것도 출력하지 않는다 (종료 코드만)
 */
#include <stdio.h>
#include <string.h>
#include "common.h"

int main(int argc, char **argv)
{
    FILE *a;
    FILE *b;
    long byte = 1;
    long line = 1;
    int silent = 0;
    int c;

    cu_prog = "cmp";
    while ((c = cu_getopt(argc, argv, "s")) != -1) {
        if (c == '?')
            return 2;
        silent = 1;
    }
    if (argc - cu_optind != 2) {
        cu_msg("usage: cmp [-s] <file1> <file2>");
        return 2;
    }
    a = strcmp(argv[cu_optind], "-") == 0 ? stdin : fopen(argv[cu_optind], "rb");
    if (!a) {
        cu_warn(argv[cu_optind]);
        return 2;
    }
    b = fopen(argv[cu_optind + 1], "rb");
    if (!b) {
        cu_warn(argv[cu_optind + 1]);
        return 2;
    }
    for (;;) {
        int x = getc(a);
        int y = getc(b);

        if (x == EOF && y == EOF)
            return 0;
        if (x == EOF || y == EOF) {
            if (!silent)
                fprintf(stderr, "cmp: EOF on %s after byte %ld\n", argv[cu_optind + (x == EOF ? 0 : 1)], byte - 1);
            return 1;
        }
        if (x != y) {
            if (!silent)
                printf("%s %s differ: byte %ld, line %ld\n", argv[cu_optind], argv[cu_optind + 1], byte, line);
            return 1;
        }
        if (x == '\n')
            line++;
        byte++;
    }
}
