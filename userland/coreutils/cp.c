/*
 * cp [-r] 원본 대상        - 파일 하나를 복사한다
 * cp [-r] 원본... 디렉터리  - 디렉터리 안으로 같은 이름으로 복사한다
 *   -r, -R  디렉터리를 내용까지 복사한다
 */
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include "common.h"

static int g_recursive = 0;

static int copy_path(const char *src, const char *dst);

static int copy_dir(const char *src, const char *dst, mode_t mode)
{
    char a[CU_PATH_MAX];
    char b[CU_PATH_MAX];
    struct dirent *e;
    size_t sl = strlen(src);
    DIR *d;
    int rc = 0;

    /* 자기 안으로 복사하면 끝나지 않는다 */
    if (strncmp(dst, src, sl) == 0 && (dst[sl] == '/' || (sl > 0 && src[sl - 1] == '/'))) {
        cu_msg("cannot copy '%s' into itself", src);
        return 1;
    }
    if (mkdir(dst, mode | 0700) != 0 && !(errno == EEXIST && cu_is_dir(dst))) {
        cu_warn(dst);
        return 1;
    }
    d = opendir(src);
    if (!d) {
        cu_warn(src);
        return 1;
    }
    while ((e = readdir(d)) != NULL) {
        if (cu_join(a, sizeof(a), src, e->d_name) < 0 || cu_join(b, sizeof(b), dst, e->d_name) < 0) {
            cu_warn(src);
            rc = 1;
            continue;
        }
        rc |= copy_path(a, b);
    }
    closedir(d);
    return rc;
}

static int copy_path(const char *src, const char *dst)
{
    struct stat st;

    if (stat(src, &st) != 0) {
        cu_warn(src);
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!g_recursive) {
            cu_msg("-r not specified; omitting directory '%s'", src);
            return 1;
        }
        return copy_dir(src, dst, (mode_t)(st.st_mode & 0777));
    }
    return cu_copy_file(src, dst) == 0 ? 0 : 1;
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

    cu_prog = "cp";
    while ((c = cu_getopt(argc, argv, "rR")) != -1) {
        if (c == '?')
            return 2;
        g_recursive = 1;
    }
    if (argc - cu_optind < 2) {
        cu_msg("usage: cp [-r] <source>... <destination>");
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
            rc |= copy_path(argv[i], target);
        } else {
            rc |= copy_path(argv[i], dst);
        }
    }
    return rc;
}
