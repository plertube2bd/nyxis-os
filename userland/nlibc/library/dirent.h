/*
 * dirent.h - 디렉터리 읽기 (NxReadDir 위)
 *
 * "." 와 ".." 는 나오지 않는다 (Nyxis 의 디렉터리 목록에 없다).
 * 커널은 "index 번째 항목" 으로 읽어 주므로, 읽는 도중 디렉터리가 바뀌면 항목을 건너뛰거나
 * 두 번 볼 수 있다. d_type (DT_REG/DT_DIR) 은 POSIX 표준은 아니지만 흔히 쓰여서 제공한다.
 */
#ifndef _NL_DIRENT_H
#define _NL_DIRENT_H

#include <sys/types.h>

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

struct dirent {
    ino_t d_ino;                /* 항상 0 이 아닌 일련번호 (Nyxis 에 inode 번호가 없다) */
    unsigned char d_type;
    char d_name[248];
};

typedef struct __nl_dir DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);
void rewinddir(DIR *dir);

#endif /* _NL_DIRENT_H */
