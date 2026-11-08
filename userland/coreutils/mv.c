/*
 * mv 원본 대상        - 이름을 바꾼다 (대상이 파일이면 덮어쓴다)
 * mv 원본... 디렉터리  - 디렉터리 안으로 옮긴다
 * 이름 바꾸기가 안 되는 곳(다른 이름공간/파일시스템 사이)의 일반 파일은 복사한 뒤 원본을 지운다.
 */
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include "common.h"

static int move_one(const char *src, const char *dst)
{
    struct stat ss;
    struct stat ds;
    int e;

    if (stat(src, &ss) != 0) {
        cu_warn(src);
        return 1;
    }
    if (rename(src, dst) == 0)
        return 0;
    e = errno;

    /* 대상이 이미 있는 파일이면 지우고 다시 (POSIX rename 은 덮어쓴다) */
    if (stat(dst, &ds) == 0 && !S_ISDIR(ds.st_mode) && !S_ISDIR(ss.st_mode)) {
        if (unlink(dst) != 0) {
            cu_warn(dst);
            return 1;
        }
        if (rename(src, dst) == 0)
            return 0;
        e = errno;
    }
    if (!S_ISDIR(ss.st_mode) && e != ENOENT && e != EACCES && e != ENAMETOOLONG && e != ENOTDIR) {
        if (cu_copy_file(src, dst) != 0)
            return 1;
        if (unlink(src) != 0) {
            cu_warn(src);
            return 1;
        }
        return 0;
    }
    errno = e;
    cu_warn(src);
    return 1;
}

int main(int argc, char **argv)
{
    char target[CU_PATH_MAX];
    char base[CU_PATH_MAX];
    const char *dst;
    int to_dir;
    int rc = 0;
    int c;
    int i;

    cu_prog = "mv";
    while ((c = cu_getopt(argc, argv, "")) != -1)
        return 2;
    if (argc - cu_optind < 2) {
        cu_msg("usage: mv <source>... <destination>");
        return 2;
    }
    dst = argv[argc - 1];
    to_dir = cu_is_dir(dst);
    if (argc - cu_optind > 2 && !to_dir) {
        cu_msg("target '%s' is not a directory", dst);
        return 1;
    }
    for (i = cu_optind; i < argc - 1; i++) {
        if (to_dir) {
            cu_basename(argv[i], base, sizeof(base));
            if (cu_join(target, sizeof(target), dst, base) < 0) {
                cu_warn(argv[i]);
                rc = 1;
                continue;
            }
            rc |= move_one(argv[i], target);
        } else {
            rc |= move_one(argv[i], dst);
        }
    }
    return rc;
}
