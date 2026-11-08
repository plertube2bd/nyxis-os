/*
 * env.c - 환경 변수 (getenv 는 C89, setenv/unsetenv 는 POSIX). environ 은 crt0 가 넘긴 배열로 시작한다.
 *
 * setenv 가 처음 바꿀 때 배열을 malloc 한 사본으로 옮기고, 그 뒤로는 그 사본을 고친다.
 * setenv 가 만든 "이름=값" 문자열은 다시 바뀌어도 해제하지 않는다 (getenv 가 돌려준 포인터가
 * 계속 유효하도록: 다른 libc 들도 같은 선택을 한다).
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nl_internal.h"

char **__nl_environ;
extern char **environ __attribute__((weak, alias("__nl_environ")));

static int env_owned;           /* __nl_environ 이 우리가 malloc 한 배열인가 */

static int valid_name(const char *name)
{
    return name != NULL && name[0] != '\0' && strchr(name, '=') == NULL;
}

char *getenv(const char *name)
{
    size_t n;
    char **e;

    if (!valid_name(name) || !__nl_environ)
        return NULL;
    n = strlen(name);
    for (e = __nl_environ; *e; e++) {
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=')
            return *e + n + 1;
    }
    return NULL;
}

static size_t env_count(void)
{
    size_t n = 0;

    while (__nl_environ && __nl_environ[n])
        n++;
    return n;
}

/* 배열을 우리 것으로 만들고 extra 칸의 여유를 둔다 */
static int own_env(size_t extra)
{
    size_t n = env_count();
    char **a = (char **)malloc((n + extra + 1) * sizeof(char *));

    if (a == NULL)
        return -1;
    if (n)
        memcpy(a, __nl_environ, n * sizeof(char *));
    a[n] = NULL;
    if (env_owned)
        free(__nl_environ);
    __nl_environ = a;
    env_owned = 1;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite)
{
    size_t nl;
    size_t n;
    size_t i;
    char *entry;

    if (!valid_name(name)) {
        errno = EINVAL;
        return -1;
    }
    nl = strlen(name);
    n = env_count();
    for (i = 0; i < n; i++) {
        if (strncmp(__nl_environ[i], name, nl) == 0 && __nl_environ[i][nl] == '=')
            break;
    }
    if (i < n && !overwrite)
        return 0;
    entry = (char *)malloc(nl + strlen(value) + 2);
    if (entry == NULL)
        return -1;
    strcpy(entry, name);
    entry[nl] = '=';
    strcpy(entry + nl + 1, value);
    if (own_env(i < n ? 0 : 1) < 0) {
        free(entry);
        return -1;
    }
    if (i < n) {
        __nl_environ[i] = entry;
    } else {
        __nl_environ[n] = entry;
        __nl_environ[n + 1] = NULL;
    }
    return 0;
}

int unsetenv(const char *name)
{
    size_t nl;
    size_t i;
    size_t j;

    if (!valid_name(name)) {
        errno = EINVAL;
        return -1;
    }
    if (!__nl_environ)
        return 0;
    if (own_env(0) < 0)
        return -1;
    nl = strlen(name);
    for (i = j = 0; __nl_environ[i]; i++) {
        if (!(strncmp(__nl_environ[i], name, nl) == 0 && __nl_environ[i][nl] == '='))
            __nl_environ[j++] = __nl_environ[i];
    }
    __nl_environ[j] = NULL;
    return 0;
}
