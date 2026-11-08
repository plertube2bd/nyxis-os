/*
 * rmdir 디렉터리... - 빈 디렉터리를 지운다
 */
#include <unistd.h>
#include "common.h"

int main(int argc, char **argv)
{
    int rc = 0;
    int c;
    int i;

    cu_prog = "rmdir";
    while ((c = cu_getopt(argc, argv, "")) != -1)
        return 2;
    if (cu_optind >= argc) {
        cu_msg("missing operand");
        return 2;
    }
    for (i = cu_optind; i < argc; i++) {
        if (rmdir(argv[i]) != 0) {
            cu_warn(argv[i]);
            rc = 1;
        }
    }
    return rc;
}
