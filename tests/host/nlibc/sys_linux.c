/*
 * sys_linux.c - nlibc 플랫폼 계층의 Linux x86_64 구현 (호스트 테스트 전용)
 *
 * userland/nlibc/src/nxsys.c 대신 링크한다: libc 의 나머지는 Nyxis 용과 똑같은 소스를
 * 그대로 쓰므로, 호스트에서 정적 바이너리로 빠르게 검사할 수 있다 (make test-nlibc).
 * Linux 에서는 핸들 = fd = (프로세스면) pid 이고, 경로의 뿌리는 "/" 다.
 */
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "../../../userland/nlibc/src/nl_internal.h"

static long lsc6(long nr, long a0, long a1, long a2, long a3, long a4)
{
    long ret;
    register long r10_ __asm__("r10") = a3;
    register long r8_ __asm__("r8") = a4;

    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10_), "r"(r8_)
                      : "rcx", "r11", "memory");
    return ret;
}

static long lsc(long nr, long a0, long a1, long a2)
{
    return lsc6(nr, a0, a1, a2, 0, 0);
}

#define L_READ 0
#define L_WRITE 1
#define L_OPEN 2
#define L_CLOSE 3
#define L_STAT 4
#define L_FSTAT 5
#define L_LSEEK 8
#define L_BRK 12
#define L_IOCTL 16
#define L_ACCESS 21
#define L_DUP 32
#define L_NANOSLEEP 35
#define L_GETPID 39
#define L_FORK 57
#define L_EXECVE 59
#define L_WAIT4 61
#define L_KILL 62
#define L_FCNTL 72
#define L_DUP2 33
#define L_CHMOD 90
#define L_UMASK 95
#define L_GETPPID 110
#define L_DUP3 292
#define L_PIPE2 293
#define L_GETCWD 79
#define L_CHDIR 80
#define L_RENAME 82
#define L_MKDIR 83
#define L_RMDIR 84
#define L_UNLINK 87
#define L_GETUID 102
#define L_GETGID 104
#define L_GETDENTS64 217
#define L_CLOCK_GETTIME 228
#define L_EXIT_GROUP 231
#define L_WAITID 247

#define LO_WRONLY 01
#define LO_RDWR 02
#define LO_CREAT 0100
#define LO_EXCL 0200
#define LO_TRUNC 01000
#define LO_APPEND 02000
#define LO_DIRECTORY 0200000

static int map_errno(long e)
{
    switch (e) {
    case 1:  return EPERM;
    case 2:  return ENOENT;
    case 3:  return ESRCH;
    case 4:  return EINTR;
    case 5:  return EIO;
    case 7:  return E2BIG;
    case 9:  return EBADF;
    case 10: return ECHILD;
    case 11: return EAGAIN;
    case 12: return ENOMEM;
    case 13: return EACCES;
    case 14: return EFAULT;
    case 16: return EBUSY;
    case 17: return EEXIST;
    case 20: return ENOTDIR;
    case 21: return EISDIR;
    case 22: return EINVAL;
    case 24: return EMFILE;
    case 25: return ENOTTY;
    case 28: return ENOSPC;
    case 29: return ESPIPE;
    case 30: return EROFS;
    case 32: return EPIPE;
    case 36: return ENAMETOOLONG;
    case 38: return ENOSYS;
    case 39: return ENOTEMPTY;
    case 95: return ENOTSUP;
    case 110: return ETIMEDOUT;
    default: return EIO;
    }
}

static long ret(long r)
{
    if (r < 0 && r > -4096) {
        errno = map_errno(-r);
        return -1;
    }
    return r;
}

long __nl_open(const char *path, int flags, unsigned int mode)
{
    long lf;

    if ((flags & NL_O_READ) && (flags & NL_O_WRITE))
        lf = LO_RDWR;
    else if (flags & NL_O_WRITE)
        lf = LO_WRONLY;
    else
        lf = 0;
    if (flags & NL_O_CREAT)
        lf |= LO_CREAT;
    if (flags & NL_O_EXCL)
        lf |= LO_EXCL;
    if (flags & NL_O_TRUNC)
        lf |= LO_TRUNC;
    if (flags & NL_O_APPEND)
        lf |= LO_APPEND;
    return ret(lsc(L_OPEN, (long)path, lf, (long)mode));
}

int __nl_handle_flags(long h)
{
    long f = ret(lsc(L_FCNTL, h, 3 /* F_GETFL */, 0));
    int r;

    if (f < 0)
        return -1;
    switch (f & 3) {
    case 0:  r = NL_O_READ; break;
    case 1:  r = NL_O_WRITE; break;
    default: r = NL_O_READ | NL_O_WRITE; break;
    }
    if (f & LO_APPEND)
        r |= NL_O_APPEND;
    return r;
}

int __nl_handle_set_append(long h, int on)
{
    long f = ret(lsc(L_FCNTL, h, 3, 0));

    if (f < 0)
        return -1;
    f = on ? (f | LO_APPEND) : (f & ~LO_APPEND);
    return (int)ret(lsc(L_FCNTL, h, 4 /* F_SETFL */, f));
}

int __nl_close(long h) { return (int)ret(lsc(L_CLOSE, h, 0, 0)); }
long __nl_read(long h, void *buf, size_t n) { return ret(lsc(L_READ, h, (long)buf, (long)n)); }
long __nl_write(long h, const void *buf, size_t n) { return ret(lsc(L_WRITE, h, (long)buf, (long)n)); }
long __nl_seek(long h, long off, int whence) { return ret(lsc(L_LSEEK, h, off, whence)); }
long __nl_dup_ex(long h, int how, int arg, int cloexec)
{
    if (how == NL_DUP_TO) {
        if (arg == h)
            return ret(lsc(L_FCNTL, h, 1, 0)) < 0 ? -1 : h;        /* dup2(fd, fd): 열려 있는지만 */
        return ret(lsc(L_DUP3, h, arg, cloexec ? 02000000 : 0));
    }
    if (how == NL_DUP_MIN)
        return ret(lsc(L_FCNTL, h, cloexec ? 1030 /* F_DUPFD_CLOEXEC */ : 0 /* F_DUPFD */, arg));
    return ret(lsc(L_FCNTL, h, cloexec ? 1030 : 0, 0));
}

int __nl_handle_cloexec(long h, int set)
{
    long f = ret(lsc(L_FCNTL, h, 1 /* F_GETFD */, 0));

    if (f < 0)
        return -1;
    if (set >= 0) {
        f = set ? (f | 1) : (f & ~1L);
        if (ret(lsc(L_FCNTL, h, 2 /* F_SETFD */, f)) < 0)
            return -1;
    }
    return (int)(f & 1);
}

/* Nyxis 처럼 "읽는 쪽 없는 파이프 쓰기 = EPIPE 오류" 가 되도록 SIGPIPE 를 무시한다 (한 번) */
static void ignore_sigpipe(void)
{
    static int done;
    unsigned long act[4];

    if (done)
        return;
    done = 1;
    act[0] = 1;                             /* SIG_IGN */
    act[1] = 0;                             /* 플래그 없음 (무시이므로 처리기 복귀 경로가 필요 없다) */
    act[2] = 0;
    act[3] = 0;
    (void)lsc6(13 /* rt_sigaction */, 13 /* SIGPIPE */, (long)act, 0, 8, 0);
}

int __nl_pipe(long out[2], int cloexec)
{
    int fds[2];

    ignore_sigpipe();

    if (ret(lsc(L_PIPE2, (long)fds, cloexec ? 02000000 : 0, 0)) < 0)
        return -1;
    out[0] = fds[0];
    out[1] = fds[1];
    return 0;
}

int __nl_chmod(const char *path, unsigned int mode) { return (int)ret(lsc(L_CHMOD, (long)path, (long)(mode & 0777), 0)); }
unsigned int __nl_umask(unsigned int mask) { return (unsigned int)lsc(L_UMASK, (long)(mask & 0777), 0, 0); }

long __nl_fd_to_handle(int fd)
{
    if (fd < 0 || lsc(L_FCNTL, fd, 3, 0) < 0) {
        errno = EBADF;
        return -1;
    }
    return fd;
}

int __nl_handle_to_fd(long h) { return (int)h; }

int __nl_is_console(long h)
{
    char termios[64];

    return lsc(L_IOCTL, h, 0x5401 /* TCGETS */, (long)termios) == 0;
}

/* 터미널: Linux struct termios (c_iflag, c_oflag, c_cflag, c_lflag, c_line, c_cc[19] ...) 의 c_lflag 만 바꾼다 */
static int linux_lflag(long h, unsigned int clear, unsigned int set)
{
    unsigned int t[16];                     /* 60 바이트면 충분 */
    long r = lsc(L_IOCTL, h, 0x5401 /* TCGETS */, (long)t);

    if (r < 0)
        return (int)ret(r);
    t[3] = (t[3] & ~clear) | set;
    if (clear & 2U) {                       /* ICANON 을 끄면: VMIN = 1, VTIME = 0 (시간 제한은 nlibc 가 poll 로) */
        unsigned char *cc = (unsigned char *)t + 17;
        cc[6] = 1;
        cc[5] = 0;
    }
    return (int)ret(lsc(L_IOCTL, h, 0x5402 /* TCSETS */, (long)t));
}

int __nl_tty_mode(long h, int raw)
{
    /* Linux: ICANON 2, ECHO 8 */
    return raw ? linux_lflag(h, 2U | 8U, 0) : linux_lflag(h, 0, 2U | 8U);
}

int __nl_tty_echo(long h, int on)
{
    return on ? linux_lflag(h, 0, 8U) : linux_lflag(h, 8U, 0);
}

int __nl_tty_winsize(long h, unsigned short ws[4])
{
    return (int)ret(lsc(L_IOCTL, h, 0x5413 /* TIOCGWINSZ */, (long)ws));
}

int __nl_wait_input(long h, long timeout_ms)
{
    struct { int fd; short events; short revents; } p;
    long r;

    p.fd = (int)h;
    p.events = 1;                           /* POLLIN */
    p.revents = 0;
    r = lsc(7 /* poll */, (long)&p, 1, timeout_ms);
    if (r < 0)
        return (int)ret(r);
    return r > 0 ? 1 : 0;
}

int __nl_poll(struct __nl_pollent *p, int n, long timeout_ms)
{
    struct { int fd; short events; short revents; } q[16];
    long r;
    int i;

    if (n <= 0 || n > 16) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < n; i++) {
        q[i].fd = (int)p[i].h;
        q[i].events = (short)(((p[i].events & NL_POLL_IN) ? 0x001 : 0) | ((p[i].events & NL_POLL_OUT) ? 0x004 : 0));
        q[i].revents = 0;
    }
    r = lsc(7 /* poll */, (long)q, n, timeout_ms < 0 ? -1L : timeout_ms);
    if (r < 0)
        return (int)ret(r);
    for (i = 0; i < n; i++) {
        p[i].revents = ((q[i].revents & 0x001) ? NL_POLL_IN : 0) |
                       ((q[i].revents & 0x004) ? NL_POLL_OUT : 0) |
                       ((q[i].revents & (0x010 | 0x008)) ? NL_POLL_HUP : 0);   /* POLLHUP | POLLERR */
    }
    return (int)r;
}

/* struct stat (x86_64): st_mode 는 24, st_size 는 48 바이트 위치 */
static void from_kstat(const unsigned char *ks, struct __nl_stat *st)
{
    unsigned int mode;
    long size;

    memcpy(&mode, ks + 24, 4);
    memcpy(&size, ks + 48, 8);
    st->mode = mode & 0777;
    memcpy(&st->uid, ks + 28, 4);
    memcpy(&st->gid, ks + 32, 4);
    switch (mode & 0170000) {
    case 0100000: st->type = NL_T_FILE; break;
    case 0040000: st->type = NL_T_DIR; break;
    case 0020000: st->type = NL_T_CHR; break;
    case 0060000: st->type = NL_T_BLK; break;
    case 0010000: st->type = NL_T_FIFO; break;
    default:      st->type = NL_T_OTHER; break;
    }
    st->size = (unsigned long)size;
}

int __nl_fstat(long h, struct __nl_stat *st)
{
    unsigned char ks[144];
    int f;

    if (ret(lsc(L_FSTAT, h, (long)ks, 0)) < 0)
        return -1;
    from_kstat(ks, st);
    f = __nl_handle_flags(h);
    st->readable = f >= 0 && (f & NL_O_READ);
    st->writable = f >= 0 && (f & NL_O_WRITE);
    return 0;
}

int __nl_stat_path(const char *path, struct __nl_stat *st)
{
    unsigned char ks[144];

    if (ret(lsc(L_STAT, (long)path, (long)ks, 0)) < 0)
        return -1;
    from_kstat(ks, st);
    st->readable = lsc(L_ACCESS, (long)path, 4, 0) == 0;
    st->writable = lsc(L_ACCESS, (long)path, 2, 0) == 0;
    return 0;
}

int __nl_mkdir(const char *path, unsigned int mode) { return (int)ret(lsc(L_MKDIR, (long)path, (long)mode, 0)); }
int __nl_rmdir(const char *path) { return (int)ret(lsc(L_RMDIR, (long)path, 0, 0)); }
int __nl_unlink(const char *path) { return (int)ret(lsc(L_UNLINK, (long)path, 0, 0)); }
int __nl_access(const char *path, int amode) { return (int)ret(lsc(L_ACCESS, (long)path, amode, 0)); }

/* index 번째 항목 ("." ".." 제외 — Nyxis 와 같게) */
int __nl_readdir(const char *path, unsigned long index, char *name, size_t cap, int *type)
{
    char buf[4096];
    long fd = ret(lsc(L_OPEN, (long)path, LO_DIRECTORY, 0));
    unsigned long seen = 0;
    long n;

    if (fd < 0)
        return -1;
    while ((n = lsc(L_GETDENTS64, fd, (long)buf, sizeof(buf))) > 0) {
        long off = 0;

        while (off < n) {
            unsigned short reclen;
            unsigned char dtype = (unsigned char)buf[off + 18];
            const char *nm = buf + off + 19;

            memcpy(&reclen, buf + off + 16, 2);
            off += reclen;
            if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
                continue;
            if (seen++ == index) {
                size_t k = strlen(nm);

                if (k >= cap)
                    k = cap - 1;
                memcpy(name, nm, k);
                name[k] = '\0';
                *type = dtype == 4 ? NL_T_DIR : dtype == 8 ? NL_T_FILE : NL_T_OTHER;
                lsc(L_CLOSE, fd, 0, 0);
                return 1;
            }
        }
    }
    lsc(L_CLOSE, fd, 0, 0);
    return n < 0 ? (int)ret(n) : 0;
}

int __nl_sleep_ms(unsigned long ms)
{
    long ts[2];

    ts[0] = (long)(ms / 1000);
    ts[1] = (long)(ms % 1000) * 1000000L;
    return (int)ret(lsc(L_NANOSLEEP, (long)ts, 0, 0));
}

int __nl_identity(unsigned int *uid, unsigned int *gid, char *name, size_t cap)
{
    *uid = (unsigned int)lsc(L_GETUID, 0, 0, 0);
    *gid = (unsigned int)lsc(L_GETGID, 0, 0, 0);
    if (name && cap)
        name[0] = '\0';             /* 이름은 모른다: getlogin 이 LOGNAME/USER 로 넘어간다 */
    return 0;
}

/* ---------------- 프로세스: fork + execve ---------------- */
/* Linux 의 fork 는 원래 fd 를 물려준다 (inherit 와 무관: FD_CLOEXEC 는 execve 가 처리) */
long __nl_spawn(const char *path, char *const argv[], char *const envp[], int inherit)
{
    long pid = lsc(L_FORK, 0, 0, 0);

    (void)inherit;
    if (pid == 0) {
        lsc(L_EXECVE, (long)path, (long)argv, (long)envp);
        lsc(L_EXIT_GROUP, 127, 0, 0);
    }
    return ret(pid);
}

long __nl_proc_pid(long h) { return h; }
long __nl_fork(void) { return ret(lsc(L_FORK, 0, 0, 0)); }
int __nl_exec(const char *path, char *const argv[], char *const envp[])
{
    return (int)ret(lsc(L_EXECVE, (long)path, (long)argv, (long)envp));
}
long __nl_getppid(void) { return lsc(L_GETPPID, 0, 0, 0); }

int __nl_proc_wait(long h, int nohang, int *wstatus)
{
    int st = 0;
    long r = ret(lsc6(L_WAIT4, h, (long)&st, nohang ? 1 /* WNOHANG */ : 0, 0, 0));

    if (r < 0)
        return -1;
    if (r == 0)
        return 0;
    *wstatus = st;
    return 1;
}

int __nl_proc_wait_any(const long *hs, int n, int nohang)
{
    unsigned char info[128];
    int pid;
    int i;
    long r;

    memset(info, 0, sizeof(info));
    /* waitid(P_ALL, 0, &info, WEXITED | WNOWAIT [| WNOHANG]): 거두지 않고 끝난 자식만 알아낸다 */
    r = ret(lsc6(L_WAITID, 0, 0, (long)info, 4 | 0x01000000 | (nohang ? 1 : 0), 0));
    if (r < 0)
        return -1;
    memcpy(&pid, info + 16, 4);
    if (pid == 0)
        return -2;
    for (i = 0; i < n; i++) {
        if (hs[i] == pid)
            return i;
    }
    errno = ECHILD;
    return -1;
}

int __nl_proc_kill(long h) { return (int)ret(lsc(L_KILL, h, 9, 0)); }

/* ---------------- 신호 (Linux rt_sig*) ---------------- */
extern char __nl_sigtramp_linux[];          /* sigtramp.s: rt_sigreturn */

struct linux_ksigaction {
    unsigned long handler;
    unsigned long flags;
    unsigned long restorer;
    unsigned long mask;
};

int __nl_sigaction(int sig, const struct __nl_sigact *act, struct __nl_sigact *old)
{
    struct linux_ksigaction k;
    struct linux_ksigaction o;
    long r;

    memset(&o, 0, sizeof(o));
    if (act) {
        k.handler = act->handler;
        k.flags = (unsigned long)(unsigned int)act->flags | 0x04000000UL;   /* SA_RESTORER */
        k.restorer = (unsigned long)__nl_sigtramp_linux;
        k.mask = act->mask;
    }
    r = ret(lsc6(13, sig, act ? (long)&k : 0, (long)&o, 8, 0));
    if (r < 0)
        return -1;
    if (old) {
        old->handler = o.handler;
        old->mask = o.mask;
        old->flags = (int)(o.flags & ~0x04000000UL);
    }
    return 0;
}

int __nl_sigprocmask(int how, const unsigned long *set, unsigned long *old)
{
    return (int)ret(lsc6(14, how, (long)set, (long)old, 8, 0));
}

int __nl_sigsend(long h, int sig)
{
    return (int)ret(lsc(L_KILL, h < 0 ? lsc(L_GETPID, 0, 0, 0) : h, sig, 0));
}

int __nl_sigpending(unsigned long *set)
{
    return (int)ret(lsc(127, (long)set, 8, 0));
}

int __nl_sigsuspend(const unsigned long *set)
{
    return (int)ret(lsc(130, (long)set, 8, 0));
}

/* Linux: setpgid(109) / getpgid(121) */
long __nl_proc_group(long h, long pgid)
{
    long pid = h < 0 ? 0 : h;

    if (pgid >= 0 && ret(lsc(109, pid, pgid, 0)) < 0)
        return -1;
    return ret(lsc(121, pid, 0, 0));
}

/* Linux: TIOCGPGRP / TIOCSPGRP (0 = "없음" 은 Linux 에 없어서 EINVAL) */
long __nl_tty_foreground(long h, long pgid)
{
    int v = (int)pgid;

    if (pgid < 0) {
        if (ret(lsc(L_IOCTL, h, 0x540F, (long)&v)) < 0)
            return -1;
        return v;
    }
    return ret(lsc(L_IOCTL, h, 0x5410, (long)&v));
}
void __nl_proc_release(long h) { (void)h; }

/* ---------------- 경로 ---------------- */
int __nl_path_is_absolute(const char *path) { return path[0] == '/'; }
size_t __nl_path_root_len(const char *abs) { return abs[0] == '/' ? 1 : 0; }

int __nl_initial_cwd(char *buf, size_t cap)
{
    return lsc(L_GETCWD, (long)buf, (long)cap, 0) > 0 ? 0 : -1;
}

void __nl_cwd_changed(const char *abs)
{
    (void)lsc(L_CHDIR, (long)abs, 0, 0);    /* 자식(fork)이 같은 디렉터리에서 시작하도록 */
}

/* 호스트에서는 환경 변수 NL_HOST_PATH 디렉터리에서만 이름으로 찾는다 (nysh 검사용). 없으면 ENOENT */
int __nl_command_path(const char *name, char *out, size_t cap)
{
    const char *dir = getenv("NL_HOST_PATH");
    size_t dl;
    size_t nl = strlen(name);

    if (dir == NULL || dir[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    dl = strlen(dir);
    if (dl + 1 + nl + 1 > cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1, name, nl + 1);
    return 0;
}

/* ---------------- 그 밖 ---------------- */
static long cur_brk;

void *__nl_sbrk(long increment)
{
    long old;
    long r;

    if (cur_brk == 0)
        cur_brk = lsc(L_BRK, 0, 0, 0);
    old = cur_brk;
    if (increment == 0)
        return (void *)old;
    r = lsc(L_BRK, old + increment, 0, 0);
    if (r != old + increment) {
        errno = ENOMEM;
        return NULL;
    }
    cur_brk = r;
    return (void *)old;
}

int __nl_remove(const char *path)
{
    long r = lsc(L_UNLINK, (long)path, 0, 0);

    if (r == -21)
        r = lsc(L_RMDIR, (long)path, 0, 0);
    return (int)ret(r);
}

int __nl_rename(const char *from, const char *to) { return (int)ret(lsc(L_RENAME, (long)from, (long)to, 0)); }

long __nl_clock_ns(int which)
{
    long ts[2];
    long r = lsc(L_CLOCK_GETTIME, which == NL_CLOCK_REALTIME ? 0 : 1, (long)ts, 0);

    if (r < 0)
        return ret(r);
    return ts[0] * 1000000000L + ts[1];
}

long __nl_getpid(void) { return lsc(L_GETPID, 0, 0, 0); }
const char *__nl_tmpdir(void) { return "/tmp/"; }

void __nl_exit(int status)
{
    (void)lsc(L_EXIT_GROUP, status, 0, 0);
    for (;;)
        ;
}
