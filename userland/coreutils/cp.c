/*
 * cp [-r] 원본 대상        - 파일 하나를 복사한다
 * cp [-r] 원본... 디렉터리  - 디렉터리 안으로 같은 이름으로 복사한다
 *   -r, -R  자식까지 복사한다 (노드의 내용과 자식 모두)
 * Nyxis 의 노드는 파일/디렉터리 구분 없이 내용과 자식을 함께 가진다 (자식이 있으면 디렉터리로 보인다).
 * -r 없이 자식이 있는 노드를 주면 내용만 복사한다 (예: cp app:/nysh.run x.run 은 실행 파일만).
 * 대상이 '/' 로 끝나면 "그 안으로" (자식이 아직 없는 노드도): cp a.txt box/
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include "common.h"

static int g_recursive = 0;

static int copy_path(const char *src, const char *dst);

/* 디렉터리가 읽을 수 있는 내용을 가졌는지 (Nyxis: 내용 없는 디렉터리는 open 이 EISDIR, 다른 OS: read 가 EISDIR) */
static int has_content(const char *path)
{
    char c;
    ssize_t n;
    int e = errno;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        errno = e;
        return 0;
    }
    n = read(fd, &c, 1);
    close(fd);
    errno = e;
    return n == 1;
}

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
    if (mkdir(dst, mode | 0700) != 0 && !(errno == EEXIST && cu_can_hold(dst))) {
        cu_warn(dst);
        return 1;
    }
    /* 노드의 내용도 (Nyxis: 자식이 있는 노드도 내용을 가진다. 다른 OS 의 디렉터리는 내용이 없다) */
    if (has_content(src) && cu_copy_file(src, dst) != 0)
        rc = 1;
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
        /* nyfs 의 디렉터리는 내용도 가질 수 있는 오브젝트다 (예: app:/nysh.run = ELF + 기본 설정).
         * -r 이 없으면 그 내용을 파일로 복사한다. 내용이 없는 디렉터리는 예전처럼 -r 이 필요하다 */
        if (!g_recursive) {
            if (has_content(src))
                return cu_copy_file(src, dst) == 0 ? 0 : 1;
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
