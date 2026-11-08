/*
 * posix_path.c - 현재 디렉터리와 경로 해석 (getcwd, chdir, __nl_resolve)
 *
 * 커널에는 현재 디렉터리가 없어서 nlibc 가 문자열로 들고 있다. 모든 경로 함수(fopen, open, stat,
 * opendir, mkdir, unlink, remove, rename, posix_spawn ...)는 커널에 넘기기 전에 __nl_resolve 로
 * 절대 경로를 만든다:
 *   - 절대 경로("tmp:/a", Linux 호스트에서는 "/a")는 그대로, 상대 경로는 현재 디렉터리 뒤에 붙인다
 *   - "." 와 빈 조각은 지우고 ".." 는 앞 조각을 지운다 (뿌리 위로는 올라가지 않는다: "tmp:/.." = "tmp:/")
 * 현재 디렉터리는 처음 쓸 때 플랫폼이 정한 값(Nyxis: 환경 변수 PWD, 없으면 face:/)으로 정해진다.
 */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nl_internal.h"

static char cwd[NL_PATH_MAX];

const char *__nl_cwd(void)
{
    if (cwd[0] == '\0' && __nl_initial_cwd(cwd, sizeof(cwd)) < 0)
        cwd[0] = '\0';
    return cwd;
}

int __nl_resolve(const char *in, char *out, size_t cap)
{
    char tmp[NL_PATH_MAX * 2];
    size_t root;
    size_t n;
    const char *p;
    char *seg_start[NL_PATH_MAX];
    int nseg = 0;
    size_t len;
    int i;

    if (in == NULL || in[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    if (__nl_path_is_absolute(in)) {
        if (strlen(in) >= sizeof(tmp)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(tmp, in);
    } else {
        const char *base = __nl_cwd();

        if (base[0] == '\0') {
            errno = ENOENT;
            return -1;
        }
        if (strlen(base) + 1 + strlen(in) >= sizeof(tmp)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(tmp, base);
        strcat(tmp, "/");
        strcat(tmp, in);
    }

    /* 뿌리("tmp:/", "/") 뒤의 조각들을 정리한다 (제자리에서) */
    root = __nl_path_root_len(tmp);
    p = tmp + root;
    while (*p) {
        const char *e = p;

        while (*e && *e != '/')
            e++;
        n = (size_t)(e - p);
        if (n == 0 || (n == 1 && p[0] == '.')) {
            /* 건너뛴다 */
        } else if (n == 2 && p[0] == '.' && p[1] == '.') {
            if (nseg > 0)
                nseg--;
        } else {
            seg_start[nseg++] = (char *)p;
        }
        p = *e ? e + 1 : e;
    }

    len = root;
    for (i = 0; i < nseg; i++) {
        const char *s = seg_start[i];

        n = 0;
        while (s[n] && s[n] != '/')
            n++;
        len += n + (i > 0 ? 1 : 0);
    }
    if (len >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, tmp, root);
    len = root;
    for (i = 0; i < nseg; i++) {
        const char *s = seg_start[i];

        if (i > 0)
            out[len++] = '/';
        while (*s && *s != '/')
            out[len++] = *s++;
    }
    out[len] = '\0';
    return 0;
}

char *getcwd(char *buf, size_t size)
{
    const char *c = __nl_cwd();
    size_t n = strlen(c);

    if (buf == NULL || size == 0) {
        errno = EINVAL;
        return NULL;
    }
    if (n == 0) {
        errno = ENOENT;
        return NULL;
    }
    if (n + 1 > size) {
        errno = ERANGE;
        return NULL;
    }
    memcpy(buf, c, n + 1);
    return buf;
}

int chdir(const char *path)
{
    char abs[NL_PATH_MAX];
    struct __nl_stat st;

    if (__nl_resolve(path, abs, sizeof(abs)) < 0)
        return -1;
    if (__nl_stat_path(abs, &st) < 0)
        return -1;
    if (st.type != NL_T_DIR) {
        errno = ENOTDIR;
        return -1;
    }
    strcpy(cwd, abs);
    __nl_cwd_changed(cwd);
    return 0;
}

char *realpath(const char *path, char *resolved)
{
    char abs[NL_PATH_MAX];
    struct __nl_stat st;
    char *out = resolved;

    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (__nl_resolve(path, abs, sizeof(abs)) < 0 || __nl_stat_path(abs, &st) < 0)
        return NULL;
    if (out == NULL) {
        out = (char *)malloc(strlen(abs) + 1);
        if (out == NULL)
            return NULL;
    }
    strcpy(out, abs);
    return out;
}
