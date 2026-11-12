/*
 * dirent.c - opendir/readdir (NxReadDir 의 "index 번째 항목" 위). dirent.h 머리말 참고.
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nl_internal.h"

struct __nl_dir {
    char path[NL_PATH_MAX];
    unsigned long index;
    struct dirent ent;
};

/*
 * path 의 자식 목록을 읽을 수 있는지 (opendir/chdir). Nyxis 의 노드는 파일/디렉터리 구분 없이 자식을 가질 수
 * 있으므로 자식이 없는 노드도 된다 (빈 목록). 다른 OS(호스트 검사)에서는 디렉터리가 아니면 ENOTDIR.
 */
int __nl_can_list(const char *abs)
{
    char name[2];
    int type;

    return __nl_readdir(abs, 0, name, sizeof(name), &type) < 0 ? -1 : 0;
}

DIR *opendir(const char *path)
{
    char abs[NL_PATH_MAX];
    struct __nl_stat st;
    DIR *d;

    if (__nl_resolve(path, abs, sizeof(abs)) < 0 || __nl_stat_path(abs, &st) < 0 || __nl_can_list(abs) < 0)
        return NULL;
    d = (DIR *)malloc(sizeof(*d));
    if (d == NULL)
        return NULL;
    strcpy(d->path, abs);
    d->index = 0;
    return d;
}

struct dirent *readdir(DIR *dir)
{
    int type;
    int r;

    if (dir == NULL) {
        errno = EBADF;
        return NULL;
    }
    r = __nl_readdir(dir->path, dir->index, dir->ent.d_name, sizeof(dir->ent.d_name), &type);
    if (r <= 0)
        return NULL;                    /* 끝이면 errno 를 건드리지 않는다 (POSIX) */
    dir->index++;
    dir->ent.d_ino = dir->index;
    dir->ent.d_type = type == NL_T_DIR ? DT_DIR : type == NL_T_FILE ? DT_REG : DT_UNKNOWN;
    return &dir->ent;
}

void rewinddir(DIR *dir)
{
    if (dir)
        dir->index = 0;
}

int closedir(DIR *dir)
{
    if (dir == NULL) {
        errno = EBADF;
        return -1;
    }
    free(dir);
    return 0;
}
