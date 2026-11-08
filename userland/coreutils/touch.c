/*
 * touch [-c] 파일... - 없는 파일을 빈 파일로 만든다.
 *   -c  만들지 않는다 (없는 파일은 조용히 넘긴다)
 * Nyxis 의 파일에는 아직 바꿀 수 있는 시각이 없어서, 이미 있는 파일은 그대로 둔다.
 */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "common.h"

int main(int argc, char **argv)
{
    struct stat st;
    int nocreate = 0;
    int rc = 0;
    int c;
    int i;

    cu_prog = "touch";
    while ((c = cu_getopt(argc, argv, "c")) != -1) {
        if (c == '?')
            return 2;
        nocreate = 1;
    }
    if (cu_optind >= argc) {
        cu_msg("missing file operand");
        return 2;
    }
    for (i = cu_optind; i < argc; i++) {
        int fd;

        if (stat(argv[i], &st) == 0)
            continue;
        if (errno != ENOENT) {
            cu_warn(argv[i]);
            rc = 1;
            continue;
        }
        if (nocreate)
            continue;
        fd = open(argv[i], O_WRONLY | O_CREAT, 0666);
        if (fd < 0) {
            cu_warn(argv[i]);
            rc = 1;
            continue;
        }
        close(fd);
    }
    return rc;
}
