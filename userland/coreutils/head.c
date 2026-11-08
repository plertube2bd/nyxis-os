/*
 * head [-n 줄수] [파일...] - 앞부분 줄을 출력한다 (기본 10줄, 파일이 없거나 "-" 이면 표준 입력)
 * 파일이 둘 이상이면 각각 앞에 "==> 이름 <==" 를 붙인다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

static int head_one(const char *path, long lines)
{
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");
    int c;

    if (!f) {
        cu_warn(path);
        return 1;
    }
    while (lines > 0 && (c = getc(f)) != EOF) {
        putchar(c);
        if (c == '\n')
            lines--;
    }
    if (ferror(f)) {
        cu_warn(path);
        lines = -1;
    }
    if (f != stdin)
        fclose(f);
    return lines < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    long lines = 10;
    char *end;
    int rc = 0;
    int c;
    int i;

    cu_prog = "head";
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
        return head_one("-", lines);
    for (i = cu_optind; i < argc; i++) {
        if (argc - cu_optind > 1)
            printf("%s==> %s <==\n", i > cu_optind ? "\n" : "", argv[i]);
        rc |= head_one(argv[i], lines);
    }
    return rc;
}
