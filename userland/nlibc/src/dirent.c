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

DIR *opendir(const char *path)
{
    char abs[NL_PATH_MAX];
    struct __nl_stat st;
    DIR *d;

    if (__nl_resolve(path, abs, sizeof(abs)) < 0 || __nl_stat_path(abs, &st) < 0)
        return NULL;
    if (st.type != NL_T_DIR) {
        errno = ENOTDIR;
        return NULL;
    }
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
