/*
 * nyxapp.c - 앱 설정 (규칙은 nyxapp.h)
 */
#include <nyxapp.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define NYXAPP_PATH_MAX 512

/* 이름 구성요소 하나: [A-Za-z0-9._-]+, "." / ".." 아님 */
static int valid_component(const char *s, size_t n)
{
    size_t i;

    if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
        return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* app 은 구성요소 하나, key 는 '/' 로 나뉜 구성요소들 */
static int valid_names(const char *app, const char *key)
{
    const char *p = key;

    if (!app || !key || !valid_component(app, strlen(app)))
        return 0;
    for (;;) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);

        if (!valid_component(p, n))
            return 0;
        if (!e)
            return 1;
        p = e + 1;
    }
}

int nyx_setting_path(int where, const char *app, const char *key, char *buf, size_t cap)
{
    int n;

    if (!buf || cap == 0 || !valid_names(app, key) || (where != NYX_SETTING_DEFAULT && where != NYX_SETTING_USER)) {
        errno = EINVAL;
        return -1;
    }
    if (where == NYX_SETTING_DEFAULT)
        n = snprintf(buf, cap, "app:/%s.run/%s", app, key);
    else
        n = snprintf(buf, cap, "roi:/%lu/app/%s/%s", (unsigned long)getuid(), app, key);
    if (n < 0 || (size_t)n >= cap) {
        errno = ERANGE;
        return -1;
    }
    return 0;
}

long nyx_setting_get_from(int where, const char *app, const char *key, char *buf, size_t cap)
{
    char path[NYXAPP_PATH_MAX];
    FILE *f;
    size_t n;
    int extra;

    if (!buf || cap == 0) {
        errno = EINVAL;
        return -1;
    }
    if (nyx_setting_path(where, app, key, path, sizeof(path)) != 0)
        return -1;
    f = fopen(path, "r");
    if (!f)
        return -1;                          /* errno: ENOENT, EACCES ... */
    n = fread(buf, 1, cap, f);
    extra = (n == cap) ? fgetc(f) : EOF;    /* cap 을 꽉 채웠다면 하나 더 있는지 */
    if (ferror(f)) {
        fclose(f);
        errno = EIO;
        return -1;
    }
    fclose(f);
    if (n > 0 && buf[n - 1] == '\n' && extra == EOF)
        n--;                                /* 끝의 개행 하나만 뗀다 */
    if (n >= cap) {                         /* NUL 자리가 없다 */
        buf[0] = '\0';
        errno = ERANGE;
        return -1;
    }
    buf[n] = '\0';
    return (long)n;
}

long nyx_setting_get(const char *app, const char *key, char *buf, size_t cap)
{
    long r = nyx_setting_get_from(NYX_SETTING_USER, app, key, buf, cap);

    if (r >= 0 || (errno != ENOENT && errno != EACCES))
        return r;                           /* 사용자 값이 있거나, 이름/크기 오류 */
    return nyx_setting_get_from(NYX_SETTING_DEFAULT, app, key, buf, cap);
}

int nyx_setting_set(const char *app, const char *key, const char *value)
{
    char path[NYXAPP_PATH_MAX];
    char *p;
    FILE *f;
    int ok;

    if (!value || nyx_setting_path(NYX_SETTING_USER, app, key, path, sizeof(path)) != 0) {
        if (!value)
            errno = EINVAL;
        return -1;
    }
    /* roi:/<uid>/app/ 다음의 각 디렉터리를 만든다 (앱 디렉터리, 키의 상위 디렉터리들) */
    p = strstr(path, "/app/");
    for (p = p ? strchr(p + 5, '/') : NULL; p; p = strchr(p + 1, '/')) {
        *p = '\0';
        if (mkdir(path, 0700) != 0 && errno != EEXIST) {
            *p = '/';
            return -1;
        }
        *p = '/';
    }
    f = fopen(path, "w");
    if (!f)
        return -1;
    ok = fputs(value, f) >= 0 && fputc('\n', f) != EOF;
    if (fclose(f) != 0)
        ok = 0;
    if (!ok) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int nyx_setting_reset(const char *app, const char *key)
{
    char path[NYXAPP_PATH_MAX];

    if (nyx_setting_path(NYX_SETTING_USER, app, key, path, sizeof(path)) != 0)
        return -1;
    if (unlink(path) != 0 && errno != ENOENT)
        return -1;
    return 0;
}
