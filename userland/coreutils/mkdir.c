/*
 * mkdir [-p] [-m 모드] 디렉터리... - 디렉터리를 만든다
 *   -p  중간 디렉터리도 만들고, 이미 있는 디렉터리는 오류로 보지 않는다
 *   -m  만든 디렉터리의 모드 (8진수, umask 와 무관하게 그대로)
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "common.h"

static int make_one(const char *path, int parents)
{
    if (mkdir(path, 0777) == 0)
        return 0;
    if (parents && errno == EEXIST && cu_can_hold(path))
        return 1;                                   /* 이미 있다: 성공, 모드는 바꾸지 않는다 */
    return -1;
}

/* -p: 앞에서부터 구성요소마다 만든다. 이름공간 접두어("tmp:/")는 건너뛴다 */
static int make_parents(const char *path)
{
    char buf[CU_PATH_MAX];
    const char *colon = strchr(path, ':');
    size_t i = colon ? (size_t)(colon - path) + 1 : 0;
    size_t len = strlen(path);

    if (len >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, len + 1);
    while (buf[i] == '/')
        i++;
    for (; i < len; i++) {
        if (buf[i] == '/' && buf[i - 1] != '/') {
            buf[i] = '\0';
            if (make_one(buf, 1) < 0)
                return -1;
            buf[i] = '/';
        }
    }
    return make_one(buf, 1);
}

int main(int argc, char **argv)
{
    int parents = 0;
    int have_mode = 0;
    unsigned long mode = 0;
    char *end;
    int rc = 0;
    int c;
    int i;

    cu_prog = "mkdir";
    while ((c = cu_getopt(argc, argv, "pm:")) != -1) {
        if (c == '?')
            return 2;
        if (c == 'p') {
            parents = 1;
        } else {
            mode = strtoul(cu_optarg, &end, 8);
            if (*cu_optarg == '\0' || *end != '\0' || mode > 0777) {
                cu_msg("invalid mode '%s'", cu_optarg);
                return 2;
            }
            have_mode = 1;
        }
    }
    if (cu_optind >= argc) {
        cu_msg("missing operand");
        return 2;
    }
    for (i = cu_optind; i < argc; i++) {
        int r = parents ? make_parents(argv[i]) : make_one(argv[i], 0);

        if (r < 0) {
            cu_warn(argv[i]);
            rc = 1;
        } else if (r == 0 && have_mode && chmod(argv[i], (mode_t)mode) != 0) {
            cu_warn(argv[i]);
            rc = 1;
        }
    }
    return rc;
}
