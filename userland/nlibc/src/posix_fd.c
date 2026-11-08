/*
 * posix_fd.c - 파일 디스크립터 I/O 와 파일시스템 함수 (unistd.h, fcntl.h, sys/stat.h)
 *
 * fd <-> 핸들 변환과 O_APPEND 흉내는 플랫폼 계층(nxsys.c)의 fd 표가 한다.
 * 경로는 모두 __nl_resolve 로 현재 디렉터리 기준의 절대 경로로 바꾼 뒤 넘긴다.
 */
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include "nl_internal.h"

static int to_nl_flags(int flags)
{
    int f = 0;

    switch (flags & O_ACCMODE) {
    case O_RDONLY: f = NL_O_READ; break;
    case O_WRONLY: f = NL_O_WRITE; break;
    case O_RDWR:   f = NL_O_READ | NL_O_WRITE; break;
    default:       return -1;
    }
    if (flags & O_CREAT)
        f |= NL_O_CREAT;
    if (flags & O_EXCL)
        f |= NL_O_EXCL;
    if ((flags & O_TRUNC) && (f & NL_O_WRITE))
        f |= NL_O_TRUNC;
    if (flags & O_APPEND)
        f |= NL_O_APPEND;
    return f;
}

int open(const char *path, int flags, ...)
{
    char abs[NL_PATH_MAX];
    unsigned int mode = 0;
    int nf = to_nl_flags(flags);
    long h;

    if (nf < 0) {
        errno = EINVAL;
        return -1;
    }
    if (flags & O_CREAT) {                  /* 세 번째 인자는 O_CREAT 일 때만 있다 */
        va_list ap;

        va_start(ap, flags);
        mode = va_arg(ap, unsigned int);
        va_end(ap);
    }
    if (__nl_resolve(path, abs, sizeof(abs)) < 0)
        return -1;
    h = __nl_open(abs, nf, mode);
    if (h < 0)
        return -1;
    if ((flags & O_CLOEXEC) && __nl_handle_cloexec(h, 1) < 0) {
        (void)__nl_close(h);
        return -1;
    }
    return __nl_handle_to_fd(h);
}

int creat(const char *path, mode_t mode)
{
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int fcntl(int fd, int cmd, ...)
{
    long h = __nl_fd_to_handle(fd);
    long n;
    int f;
    int arg = 0;
    va_list ap;

    if (h < 0)
        return -1;
    if (cmd == F_SETFD || cmd == F_SETFL || cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
        va_start(ap, cmd);
        arg = va_arg(ap, int);
        va_end(ap);
    }
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        if (arg < 0) {
            errno = EINVAL;
            return -1;
        }
        n = __nl_dup_ex(h, NL_DUP_MIN, arg, cmd == F_DUPFD_CLOEXEC);
        return n < 0 ? -1 : __nl_handle_to_fd(n);
    case F_GETFD:
        f = __nl_handle_cloexec(h, -1);
        return f < 0 ? -1 : (f ? FD_CLOEXEC : 0);
    case F_SETFD:
        return __nl_handle_cloexec(h, (arg & FD_CLOEXEC) != 0) < 0 ? -1 : 0;
    case F_GETFL:
        f = __nl_handle_flags(h);
        if (f < 0)
            return -1;
        return ((f & NL_O_READ) && (f & NL_O_WRITE) ? O_RDWR : (f & NL_O_WRITE) ? O_WRONLY : O_RDONLY)
               | ((f & NL_O_APPEND) ? O_APPEND : 0);
    case F_SETFL:                           /* 바꿀 수 있는 것은 O_APPEND 뿐 */
        return __nl_handle_set_append(h, (arg & O_APPEND) != 0);
    default:
        errno = EINVAL;
        return -1;
    }
}

/* termios.c 의 tcsetattr 가 정한다: 비정규 모드 + VMIN == 0 이면 콘솔 읽기 전에 기다릴 최대 시간(ms), 아니면 -1 */
long __nl_tty_read_timeout_ms = -1;

ssize_t read(int fd, void *buf, size_t n)
{
    long h = __nl_fd_to_handle(fd);

    if (h < 0)
        return -1;
    /* VMIN = 0, VTIME = t: 입력이 t/10 초 안에 오지 않으면 0 을 돌려준다 */
    if (__nl_tty_read_timeout_ms >= 0 && n > 0 && __nl_is_console(h)) {
        int r = __nl_wait_input(h, __nl_tty_read_timeout_ms);

        if (r <= 0)
            return r;
    }
    return __nl_read(h, buf, n);
}

ssize_t write(int fd, const void *buf, size_t n)
{
    long h = __nl_fd_to_handle(fd);

    if (h < 0)
        return -1;
    return __nl_write(h, buf, n);
}

int close(int fd)
{
    long h = __nl_fd_to_handle(fd);

    if (h < 0)
        return -1;
    return __nl_close(h);
}

off_t lseek(int fd, off_t offset, int whence)
{
    long h = __nl_fd_to_handle(fd);

    if (h < 0)
        return -1;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        errno = EINVAL;
        return -1;
    }
    return __nl_seek(h, offset, whence);
}

int dup(int fd)
{
    long h = __nl_fd_to_handle(fd);
    long n;

    if (h < 0)
        return -1;
    n = __nl_dup_ex(h, NL_DUP_ANY, 0, 0);
    if (n < 0)
        return -1;
    return __nl_handle_to_fd(n);
}

/* 열린 파일을 공유하는 fd2 를 만든다. fd2 가 열려 있으면 먼저 닫는다 (커널이 한 번에) */
int dup2(int fd, int fd2)
{
    long h = __nl_fd_to_handle(fd);
    long n;

    if (h < 0)
        return -1;
    if (fd2 < 0 || fd2 >= 32) {             /* NX_MAX_HANDLES */
        errno = EBADF;
        return -1;
    }
    if (fd == fd2)
        return fd2;
    n = __nl_dup_ex(h, NL_DUP_TO, fd2, 0);
    if (n < 0)
        return -1;
    return __nl_handle_to_fd(n);
}

int pipe(int fds[2])
{
    long h[2];

    if (__nl_pipe(h, 0) < 0)
        return -1;
    fds[0] = __nl_handle_to_fd(h[0]);
    fds[1] = __nl_handle_to_fd(h[1]);
    return 0;
}

int isatty(int fd)
{
    long h = __nl_fd_to_handle(fd);

    if (h < 0)
        return 0;
    if (!__nl_is_console(h)) {
        errno = ENOTTY;
        return 0;
    }
    return 1;
}

static void fill_stat(const struct __nl_stat *s, struct stat *b)
{
    memset(b, 0, sizeof(*b));
    switch (s->type) {
    case NL_T_FILE: b->st_mode = S_IFREG; break;
    case NL_T_DIR:  b->st_mode = S_IFDIR; break;
    case NL_T_CHR:  b->st_mode = S_IFCHR; break;
    case NL_T_BLK:  b->st_mode = S_IFBLK; break;
    case NL_T_FIFO: b->st_mode = S_IFIFO; break;
    default:        b->st_mode = 0; break;
    }
    if (s->mode != 0 || s->type == NL_T_FILE || s->type == NL_T_DIR) {
        b->st_mode |= s->mode & 0777;       /* 커널이 알려 준 권한 비트 */
    } else {
        /* 권한 비트가 없는 종류(콘솔, 장치): 이 프로세스가 할 수 있는 일로 채운다 */
        if (s->readable)
            b->st_mode |= S_IRUSR | S_IRGRP | S_IROTH;
        if (s->writable)
            b->st_mode |= S_IWUSR | S_IWGRP | S_IWOTH;
    }
    b->st_uid = s->uid;
    b->st_gid = s->gid;
    b->st_nlink = 1;
    b->st_size = (off_t)s->size;
    b->st_blksize = 512;
    b->st_blocks = (long)((s->size + 511) / 512);
}

int stat(const char *path, struct stat *buf)
{
    char abs[NL_PATH_MAX];
    struct __nl_stat s;

    if (__nl_resolve(path, abs, sizeof(abs)) < 0 || __nl_stat_path(abs, &s) < 0)
        return -1;
    fill_stat(&s, buf);
    return 0;
}

int fstat(int fd, struct stat *buf)
{
    long h = __nl_fd_to_handle(fd);
    struct __nl_stat s;

    if (h < 0 || __nl_fstat(h, &s) < 0)
        return -1;
    fill_stat(&s, buf);
    return 0;
}

int mkdir(const char *path, mode_t mode)
{
    char abs[NL_PATH_MAX];

    if (__nl_resolve(path, abs, sizeof(abs)) < 0)
        return -1;
    return __nl_mkdir(abs, mode);
}

int chmod(const char *path, mode_t mode)
{
    char abs[NL_PATH_MAX];

    if (mode & ~0777U) {
        errno = EINVAL;                     /* setuid/setgid/sticky 는 지원하지 않는다 */
        return -1;
    }
    if (__nl_resolve(path, abs, sizeof(abs)) < 0)
        return -1;
    return __nl_chmod(abs, mode);
}

mode_t umask(mode_t mask)
{
    return (mode_t)__nl_umask(mask & 0777U);
}

int rmdir(const char *path)
{
    char abs[NL_PATH_MAX];

    if (__nl_resolve(path, abs, sizeof(abs)) < 0)
        return -1;
    return __nl_rmdir(abs);
}

int unlink(const char *path)
{
    char abs[NL_PATH_MAX];

    if (__nl_resolve(path, abs, sizeof(abs)) < 0)
        return -1;
    return __nl_unlink(abs);
}

int access(const char *path, int mode)
{
    char abs[NL_PATH_MAX];

    if (mode & ~(R_OK | W_OK | X_OK)) {
        errno = EINVAL;
        return -1;
    }
    if (__nl_resolve(path, abs, sizeof(abs)) < 0)
        return -1;
    return __nl_access(abs, mode);
}
