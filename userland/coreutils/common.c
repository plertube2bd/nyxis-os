/*
 * common.c - coreutils 공용 도우미 (common.h 참고)
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "common.h"

const char *cu_prog = "?";
int cu_optind = 1;
char *cu_optarg = NULL;

static int g_optpos = 0;        /* 묶인 옵션("-rf") 안에서 다음 글자 위치 */

int cu_getopt(int argc, char **argv, const char *spec)
{
    char *arg;
    const char *p;
    int c;

    cu_optarg = NULL;
    if (cu_optind >= argc)
        return -1;
    arg = argv[cu_optind];
    if (g_optpos == 0) {
        if (arg[0] != '-' || arg[1] == '\0')
            return -1;
        if (strcmp(arg, "--") == 0) {
            cu_optind++;
            return -1;
        }
        g_optpos = 1;
    }

    c = (unsigned char)arg[g_optpos++];
    p = (c == ':') ? NULL : strchr(spec, c);
    if (!p) {
        cu_msg("unknown option -%c", c);
        if (arg[g_optpos] == '\0') {
            cu_optind++;
            g_optpos = 0;
        }
        return '?';
    }
    if (p[1] == ':') {
        if (arg[g_optpos] != '\0') {
            cu_optarg = arg + g_optpos;
        } else if (cu_optind + 1 < argc) {
            cu_optarg = argv[++cu_optind];
        } else {
            cu_msg("option -%c needs a value", c);
            cu_optind++;
            g_optpos = 0;
            return '?';
        }
        cu_optind++;
        g_optpos = 0;
        return c;
    }
    if (arg[g_optpos] == '\0') {
        cu_optind++;
        g_optpos = 0;
    }
    return c;
}

void cu_warn(const char *what)
{
    int e = errno;

    fprintf(stderr, "%s: %s: %s\n", cu_prog, what, strerror(e));
}

void cu_msg(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: ", cu_prog);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void cu_basename(const char *path, char *out, size_t cap)
{
    const char *colon = strchr(path, ':');
    const char *start = colon ? colon + 1 : path;   /* 이름공간 접두어는 구성요소가 아니다 */
    size_t end = strlen(path);
    size_t b;
    size_t n;

    while (end > (size_t)(start - path) && path[end - 1] == '/')
        end--;
    b = end;
    while (b > (size_t)(start - path) && path[b - 1] != '/')
        b--;
    n = end - b;
    if (cap == 0)
        return;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, path + b, n);
    out[n] = '\0';
}

int cu_join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    int slash = (dl > 0 && dir[dl - 1] != '/');

    if (dl + (size_t)slash + nl + 1 > cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, dir, dl);
    if (slash)
        out[dl++] = '/';
    memcpy(out + dl, name, nl + 1);
    return 0;
}

int cu_can_hold(const char *path)
{
    int e = errno;
    DIR *d = opendir(path);

    if (d)
        closedir(d);
    errno = e;
    return d != NULL;
}

int cu_is_dir(const char *path)
{
    struct stat st;
    int e = errno;
    size_t n = strlen(path);
    int r = stat(path, &st) == 0 && S_ISDIR(st.st_mode);

    errno = e;
    /* 끝의 '/' 는 "그 안으로" 라는 뜻: 자식이 아직 없는 노드도 담는 곳으로 (Nyxis 노드는 무엇이든 자식을 가진다) */
    if (!r && n > 1 && path[n - 1] == '/')
        r = cu_can_hold(path);
    return r;
}

int cu_write_all(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;

    while (n > 0) {
        ssize_t w = write(fd, p, n);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int cu_copy_fd(int in, int out)
{
    char buf[4096];
    ssize_t n;

    while ((n = read(in, buf, sizeof(buf))) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (cu_write_all(out, buf, (size_t)n) < 0)
            return -1;
    }
    return 0;
}

int cu_copy_file(const char *src, const char *dst)
{
    struct stat st;
    int in;
    int out;
    int r;

    in = open(src, O_RDONLY);
    if (in < 0 || fstat(in, &st) != 0) {
        cu_warn(src);
        if (in >= 0)
            close(in);
        return -1;
    }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)(st.st_mode & 0777));
    if (out < 0) {
        cu_warn(dst);
        close(in);
        return -1;
    }
    r = cu_copy_fd(in, out);
    if (r < 0)
        cu_warn(dst);
    close(in);
    if (close(out) != 0 && r == 0) {
        cu_warn(dst);
        r = -1;
    }
    return r;
}

int cu_color_ok(int fd)
{
    const char *nc = getenv("NO_COLOR");
    const char *term = getenv("TERM");

    if (nc && nc[0])
        return 0;
    if (term && strcmp(term, "dumb") == 0)
        return 0;
    return isatty(fd);
}
