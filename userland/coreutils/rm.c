/*
 * rm [-rf] 경로... - 파일을 지운다
 *   -r, -R  디렉터리를 안의 내용까지 지운다
 *   -f      없는 파일은 조용히 넘기고, 피연산자가 없어도 오류가 아니다
 * 이름공간 뿌리("tmp:/"), ".", ".." 는 지우지 않는다.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "common.h"

static int g_recursive = 0;
static int g_force = 0;

static int remove_path(const char *path);

/*
 * 디렉터리 안을 비운다. 목록은 "n 번째 항목" 으로 읽히므로 지우면서 읽으면 항목을 건너뛴다:
 * 그래서 매번 처음부터 다시 열어 (지우지 못한 skip 개를 건너뛴) 첫 항목을 지운다.
 */
static int empty_dir(const char *path)
{
    char child[CU_PATH_MAX];
    int skip = 0;
    int rc = 0;

    for (;;) {
        DIR *d = opendir(path);
        struct dirent *e = NULL;
        int k;

        if (!d) {
            cu_warn(path);
            return 1;
        }
        for (k = 0; k <= skip; k++) {
            e = readdir(d);
            if (!e)
                break;
        }
        if (!e) {
            closedir(d);
            return rc;
        }
        if (cu_join(child, sizeof(child), path, e->d_name) < 0) {
            closedir(d);
            cu_warn(path);
            return 1;
        }
        closedir(d);
        if (remove_path(child) != 0) {
            rc = 1;
            skip++;
        }
    }
}

static int remove_path(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        if (g_force && errno == ENOENT)
            return 0;
        cu_warn(path);
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!g_recursive) {
            errno = EISDIR;
            cu_warn(path);
            return 1;
        }
        if (empty_dir(path) != 0)
            return 1;
        if (rmdir(path) != 0) {
            cu_warn(path);
            return 1;
        }
        return 0;
    }
    if (unlink(path) != 0) {
        cu_warn(path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char base[CU_PATH_MAX];
    int rc = 0;
    int c;
    int i;

    cu_prog = "rm";
    while ((c = cu_getopt(argc, argv, "rRf")) != -1) {
        if (c == '?')
            return 2;
        if (c == 'f')
            g_force = 1;
        else
            g_recursive = 1;
    }
    if (cu_optind >= argc) {
        if (g_force)
            return 0;
        cu_msg("missing operand");
        return 2;
    }
    for (i = cu_optind; i < argc; i++) {
        cu_basename(argv[i], base, sizeof(base));
        if (base[0] == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
            cu_msg("refusing to remove '%s'", argv[i]);
            rc = 1;
            continue;
        }
        rc |= remove_path(argv[i]);
    }
    return rc;
}
