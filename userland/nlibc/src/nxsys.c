/*
 * nxsys.c - nlibc 플랫폼 계층의 Nyxis 구현 (경계는 nl_internal.h)
 *
 * 시스템 콜은 `syscall` 명령으로 부른다 (include/nyx_abi.h 의 호출 규약).
 * 음수 반환값은 Nstatus 이며, 여기서 errno 로 바꾼다 (변환표의 주인은 nlibc: errno.h 머리말).
 *
 * fd 표: fd 는 핸들의 슬롯 번호(하위 32비트)다. 커널은 가장 낮은 빈 슬롯을 주므로 POSIX 의
 * "가장 낮은 fd" 규칙이 저절로 지켜진다. 표에는 세대까지 담긴 전체 핸들 값과 열기 플래그를 둔다
 * (닫힌 뒤 재사용된 슬롯의 옛 핸들을 커널이 거부하므로, 표가 틀려도 엉뚱한 파일을 건드리지 않는다).
 */
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <nyx_abi.h>
#include "nl_internal.h"

static long nx_sc5(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2,
                   unsigned long a3, unsigned long a4)
{
    unsigned long ret;
    register unsigned long r10_ __asm__("r10") = a3;
    register unsigned long r8_  __asm__("r8")  = a4;
    register unsigned long r9_  __asm__("r9")  = 0;

    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10_), "r"(r8_), "r"(r9_)
        : "rcx", "r11", "memory"
    );
    return (long)ret;
}

static long nx_sc(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2)
{
    return nx_sc5(nr, a0, a1, a2, 0, 0);
}

#define NST_NOT_FOUND       1U
#define NST_ALREADY_EXISTS  7U
#define NST_TIMEOUT         5U

/* 음수 반환값에서 양의 Nstatus 코드 */
static unsigned int nst_code(long r)
{
    return (unsigned int)r & 0x7FFFFFFFU;
}

static int errno_from_status(unsigned int code)
{
    switch (code) {
    case 1:  return ENOENT;      /* NnotFound */
    case 2:  return EINVAL;      /* NinvalidArg */
    case 3:  return ENOMEM;      /* NoutOfMemory */
    case 4:  return EACCES;      /* Npermission */
    case 5:  return ETIMEDOUT;   /* Ntimeout */
    case 6:  return EAGAIN;      /* Nbusy */
    case 7:  return EEXIST;      /* NalreadyExists */
    case 8:  return ERANGE;      /* Noverflow */
    case 10: return EIO;         /* Nio */
    case 11: return EPIPE;       /* Ndisconnected: 상대(읽는 쪽/수신 끝)가 사라졌다 */
    case 12: return ENOTSUP;     /* Nunsupported */
    case 14: return EINTR;       /* Ninterrupted: 신호가 잠든 호출을 깨웠다 */
    case 21: return EFAULT;      /* NnullPointer */
    case 22: return EFAULT;      /* NinvalidPointer */
    case 27: return ERANGE;      /* NbufferOverflow */
    case 29: return ENOENT;      /* NfileNotFound */
    case 30: return ENAMETOOLONG; /* NpathTooLong */
    case 31: return EROFS;       /* NreadOnly */
    case 32: return ENOSPC;      /* NdiskFull */
    case 44: return EACCES;      /* Nunauthorized */
    case 45: return EACCES;      /* Nauthentication */
    case 47: return EINVAL;      /* NinvalidFormat */
    case 53: return EBUSY;       /* NdeviceBusy */
    case 58: return ENOSYS;      /* NsyscallFailed */
    default: return EIO;
    }
}

/* 시스템 콜 결과를 nlibc 규약(실패 = -1 + errno)으로 */
static long ret(long r)
{
    if (r < 0) {
        errno = errno_from_status(nst_code(r));
        return -1;
    }
    return r;
}

/* ---------------- fd 표 ---------------- */
struct fd_entry {
    long handle;        /* 전체 핸들 값 + 1 (0 = 비어 있음) */
    int flags;          /* NL_O_* */
};

static struct fd_entry fds[NX_MAX_HANDLES] = {
    { (long)NX_HANDLE_STDIN + 1, NL_O_READ },
    { (long)NX_HANDLE_STDOUT + 1, NL_O_WRITE },
    { (long)NX_HANDLE_STDERR + 1, NL_O_WRITE },
};

static int slot_of(long h)
{
    unsigned long s = (unsigned long)h & 0xFFFFFFFFUL;

    return s < NX_MAX_HANDLES ? (int)s : -1;
}

static struct fd_entry *entry_of(long h)
{
    int s = slot_of(h);

    if (s < 0 || fds[s].handle != h + 1)
        return NULL;
    return &fds[s];
}

static void fd_register(long h, int flags)
{
    int s = slot_of(h);

    if (s >= 0) {
        fds[s].handle = h + 1;
        fds[s].flags = flags;
    }
}

/*
 * 표에 없는 fd 는 "세대 0 핸들" 로 확인해 본다: 부모에게 물려받은 fd(NX_SPAWN_INHERIT)와 exec 뒤에 남은 fd 는
 * 커널이 세대를 0 으로 맞춰 주므로 핸들 값 = fd 다. 열린 핸들이면 권한으로 플래그를 짐작해 등록한다.
 */
long __nl_fd_to_handle(int fd)
{
    struct nx_stat st;

    if (fd < 0 || fd >= (int)NX_MAX_HANDLES) {
        errno = EBADF;
        return -1;
    }
    if (fds[fd].handle == 0) {
        if (nx_sc(NX_SYS_STAT, (unsigned long)fd, (unsigned long)&st, 0) < 0
            || st.type == NX_TYPE_PROCESS || st.type == NX_TYPE_TOKEN) {
            errno = EBADF;
            return -1;
        }
        fds[fd].handle = (long)fd + 1;
        fds[fd].flags = ((st.rights & NX_RIGHT_READ) ? NL_O_READ : 0) | ((st.rights & NX_RIGHT_WRITE) ? NL_O_WRITE : 0);
    }
    return fds[fd].handle - 1;
}

int __nl_handle_to_fd(long h)
{
    return entry_of(h) ? slot_of(h) : -1;
}

int __nl_handle_flags(long h)
{
    struct fd_entry *e = entry_of(h);

    if (!e) {
        errno = EBADF;
        return -1;
    }
    return e->flags;
}

int __nl_handle_set_append(long h, int on)
{
    struct fd_entry *e = entry_of(h);

    if (!e) {
        errno = EBADF;
        return -1;
    }
    e->flags = on ? (e->flags | NL_O_APPEND) : (e->flags & ~NL_O_APPEND);
    return 0;
}

/* ---------------- 파일 ---------------- */
static long raw_open(const char *path, int flags)
{
    unsigned long nf = 0;

    if (flags & NL_O_READ)
        nf |= NX_O_READ;
    if (flags & NL_O_WRITE)
        nf |= NX_O_WRITE;
    if (flags & NL_O_APPEND)
        nf |= NX_O_APPEND;      /* 커널이 열린 파일에 기억한다: exec 한 프로그램(셸의 >>)에도 이어진다 */
    if ((flags & NL_O_TRUNC) && (flags & NL_O_WRITE))
        nf |= NX_O_TRUNC;       /* 커널이 내용만 자른다 (모드/소유자/자식은 그대로. 장치에서는 무시) */
    return nx_sc(NX_SYS_OPEN, (unsigned long)path, nf, 0);
}

static long raw_create(const char *path, unsigned int mode)
{
    return nx_sc(NX_SYS_CREATE_NODE, (unsigned long)path, NX_NODE_FILE, mode & 0777U);
}

static long open_registered(const char *path, int flags)
{
    long h = raw_open(path, flags);

    if (h >= 0)
        fd_register(h, flags);
    return ret(h);
}

/*
 * 새 파일을 만들고 연다. POSIX 는 O_CREAT 로 "새로 만든" 파일을 모드와 상관없이 요청한 접근으로 열어 준다
 * (open(p, O_CREAT | O_WRONLY, 0444) 도 쓸 수 있는 fd 를 준다: cp 가 읽기 전용 파일을 복사할 때 필요).
 * 만들기와 열기가 따로라서, 소유자 읽기/쓰기를 더해 만들고 연 뒤 요청한 모드로 되돌린다.
 * may_exist: 그사이 다른 쪽이 먼저 만들었으면(NST_ALREADY_EXISTS) 그 파일을 그냥 연다.
 */
static long create_open(const char *path, int flags, unsigned int mode, int may_exist)
{
    struct nx_stat st;
    long r = raw_create(path, mode | 0600U);
    long h;

    if (r < 0) {
        if (may_exist && nst_code(r) == NST_ALREADY_EXISTS)
            return open_registered(path, flags);
        return ret(r);
    }
    h = raw_open(path, flags);
    /* 만들어진 모드 = (mode | 0600) & ~umask. 더했던 비트만 뺀다 (umask 는 커널이 이미 적용했다) */
    if ((mode & 0600U) != 0600U && nx_sc(NX_SYS_STAT_PATH, (unsigned long)path, (unsigned long)&st, 0) >= 0)
        (void)nx_sc(NX_SYS_SET_MODE, (unsigned long)path, st.mode & 0777U & ~(0600U & ~mode), 0);
    if (h >= 0)
        fd_register(h, flags);
    return ret(h);
}

/*
 * 노드는 파일/디렉터리 구분이 없는 하나의 오브젝트다 (ABI 2.5): 어느 노드든 open/read/write 로 내용을 다루고,
 * 자식이 있어도 그대로 열린다. 자르기는 커널(NX_O_TRUNC)이, 만들기는 NxCreateNode 로 한다.
 */
long __nl_open(const char *path, int flags, unsigned int mode)
{
    long h;

    if ((flags & (NL_O_CREAT | NL_O_EXCL)) == (NL_O_CREAT | NL_O_EXCL)) {
        return create_open(path, flags, mode, 0);   /* 만들기가 곧 "없었음" 의 확인이다 (원자적) */
    }

    h = raw_open(path, flags);
    if (h < 0) {
        if (!(flags & NL_O_CREAT) || nst_code(h) != NST_NOT_FOUND)
            return ret(h);
        return create_open(path, flags, mode, 1);
    }
    fd_register(h, flags);
    return h;
}

int __nl_close(long h)
{
    struct fd_entry *e = entry_of(h);

    if (e)
        e->handle = 0;
    return (int)ret(nx_sc(NX_SYS_CLOSE, (unsigned long)h, 0, 0));
}

long __nl_read(long h, void *buf, size_t n)
{
    return ret(nx_sc(NX_SYS_READ, (unsigned long)h, (unsigned long)buf, n));
}

long __nl_write(long h, const void *buf, size_t n)
{
    struct fd_entry *e = entry_of(h);

    if (e && (e->flags & NL_O_APPEND))
        (void)nx_sc(NX_SYS_SEEK, (unsigned long)h, 0, NX_SEEK_END);
    return ret(nx_sc(NX_SYS_WRITE, (unsigned long)h, (unsigned long)buf, n));
}

long __nl_seek(long h, long offset, int whence)
{
    struct nx_stat st;

    /* 콘솔/채널처럼 위치가 없는 핸들은 POSIX 대로 ESPIPE */
    if (nx_sc(NX_SYS_STAT, (unsigned long)h, (unsigned long)&st, 0) >= 0
        && st.type != NX_TYPE_FILE && st.type != NX_TYPE_DEVICE) {
        errno = ESPIPE;
        return -1;
    }
    return ret(nx_sc(NX_SYS_SEEK, (unsigned long)h, (unsigned long)offset, (unsigned long)whence));
}

int __nl_is_console(long h)
{
    struct nx_stat st;

    if (nx_sc(NX_SYS_STAT, (unsigned long)h, (unsigned long)&st, 0) < 0)
        return 0;
    return st.type == NX_TYPE_CONSOLE;
}

int __nl_tty_mode(long h, int raw)
{
    return (int)ret(nx_sc(NX_SYS_DEVICE_COMMAND, (unsigned long)h, NX_DEVCMD_TTY_MODE,
                          raw ? NX_TTY_MODE_RAW : NX_TTY_MODE_COOKED));
}

int __nl_tty_echo(long h, int on)
{
    return (int)ret(nx_sc(NX_SYS_DEVICE_COMMAND, (unsigned long)h, NX_DEVCMD_TTY_ECHO, on ? 1UL : 0UL));
}

int __nl_tty_winsize(long h, unsigned short ws[4])
{
    struct nx_winsize w;
    long r = nx_sc(NX_SYS_DEVICE_COMMAND, (unsigned long)h, NX_DEVCMD_TTY_WINSIZE, (unsigned long)&w);

    if (r < 0)
        return (int)ret(r);
    ws[0] = w.rows;
    ws[1] = w.cols;
    ws[2] = w.xpixel;
    ws[3] = w.ypixel;
    return 0;
}

int __nl_wait_input(long h, long timeout_ms)
{
    struct nx_pollfd p;
    long r;

    p.handle = (unsigned long)h;
    p.events = NX_POLL_IN;
    p.revents = 0;
    r = nx_sc(NX_SYS_POLL, (unsigned long)&p, 1, (unsigned long)timeout_ms);
    if (r < 0)
        return (int)ret(r);
    return r > 0 ? 1 : 0;
}

int __nl_poll(struct __nl_pollent *p, int n, long timeout_ms)
{
    struct nx_pollfd q[NX_POLL_MAX];
    long r;
    int i;

    if (n <= 0 || n > (int)NX_POLL_MAX) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < n; i++) {
        q[i].handle = (unsigned long)p[i].h;
        q[i].events = ((p[i].events & NL_POLL_IN) ? NX_POLL_IN : 0U) |
                      ((p[i].events & NL_POLL_OUT) ? NX_POLL_OUT : 0U);
        q[i].revents = 0;
    }
    r = nx_sc(NX_SYS_POLL, (unsigned long)q, (unsigned long)n,
              timeout_ms < 0 ? (unsigned long)NX_WAIT_INFINITE : (unsigned long)timeout_ms);
    if (r < 0)
        return (int)ret(r);
    for (i = 0; i < n; i++) {
        p[i].revents = ((q[i].revents & NX_POLL_IN) ? NL_POLL_IN : 0) |
                       ((q[i].revents & NX_POLL_OUT) ? NL_POLL_OUT : 0) |
                       ((q[i].revents & NX_POLL_HUP) ? NL_POLL_HUP : 0);
    }
    return (int)r;
}

long __nl_dup_ex(long h, int how, int arg, int cloexec)
{
    struct fd_entry *e = entry_of(h);
    int flags = e ? e->flags : (NL_O_READ | NL_O_WRITE);
    unsigned long f = NX_DUP_SHARE | (cloexec ? NX_DUP_CLOEXEC : 0)
                    | (how == NL_DUP_TO ? NX_DUP_TO_SLOT : how == NL_DUP_MIN ? NX_DUP_MIN_SLOT : 0);
    long n = nx_sc5(NX_SYS_DUPLICATE_HANDLE_EX, (unsigned long)h, NX_RIGHT_ALL, f, (unsigned long)arg, 0);

    if (n < 0)
        return ret(n);
    if (n != h)
        fd_register(n, flags);
    return n;
}

int __nl_handle_cloexec(long h, int set)
{
    long r;

    if (set < 0)
        r = nx_sc(NX_SYS_HANDLE_FLAGS, (unsigned long)h, 0, 0);
    else
        r = nx_sc(NX_SYS_HANDLE_FLAGS, (unsigned long)h, set ? NX_HFLAG_CLOEXEC : 0, set ? 0 : NX_HFLAG_CLOEXEC);
    if (r < 0)
        return (int)ret(r);
    return (r & NX_HFLAG_CLOEXEC) != 0;
}

int __nl_pipe(long out[2], int cloexec)
{
    unsigned long h[2];
    long r = nx_sc(NX_SYS_PIPE_CREATE, (unsigned long)h, cloexec ? NX_DUP_CLOEXEC : 0, 0);

    if (r < 0)
        return (int)ret(r);
    fd_register((long)h[0], NL_O_READ);
    fd_register((long)h[1], NL_O_WRITE);
    out[0] = (long)h[0];
    out[1] = (long)h[1];
    return 0;
}

int __nl_openpty(long out[2], int cloexec)
{
    unsigned long h[2];
    long r = nx_sc(NX_SYS_PTY_CREATE, (unsigned long)h, cloexec ? NX_DUP_CLOEXEC : 0, 0);

    if (r < 0)
        return (int)ret(r);
    fd_register((long)h[0], NL_O_READ | NL_O_WRITE);
    fd_register((long)h[1], NL_O_READ | NL_O_WRITE);
    out[0] = (long)h[0];
    out[1] = (long)h[1];
    return 0;
}

int __nl_tty_set_winsize(long h, const unsigned short ws[4])
{
    struct nx_winsize w;

    w.rows = ws[0];
    w.cols = ws[1];
    w.xpixel = ws[2];
    w.ypixel = ws[3];
    return (int)ret(nx_sc(NX_SYS_DEVICE_COMMAND, (unsigned long)h, NX_DEVCMD_TTY_SET_WINSIZE, (unsigned long)&w));
}

long __nl_devcmd(long h, unsigned long cmd, unsigned long arg)
{
    return ret(nx_sc(NX_SYS_DEVICE_COMMAND, (unsigned long)h, cmd, arg));
}

void *__nl_map_mmio(long h)
{
    unsigned long va = 0;
    long r = nx_sc(NX_SYS_DEVICE_MAP_MMIO, (unsigned long)h, (unsigned long)&va, 0);

    if (r < 0) {
        (void)ret(r);
        return NULL;
    }
    return (void *)va;
}

static int type_of(unsigned int t)
{
    switch (t) {
    case NX_TYPE_FILE:    return NL_T_FILE;
    case NX_TYPE_DIR:     return NL_T_DIR;
    case NX_TYPE_CONSOLE: return NL_T_CHR;
    case NX_TYPE_CHANNEL: return NL_T_FIFO;
    case NX_TYPE_DEVICE:  return NL_T_CHR;
    case NX_TYPE_PTY:     return NL_T_CHR;
    default:              return NL_T_OTHER;
    }
}

int __nl_fstat(long h, struct __nl_stat *out)
{
    struct nx_stat st;
    long r = nx_sc(NX_SYS_STAT, (unsigned long)h, (unsigned long)&st, 0);

    if (r < 0)
        return (int)ret(r);
    out->type = type_of(st.type);
    out->size = st.size;
    out->readable = (st.rights & NX_RIGHT_READ) != 0;
    out->writable = (st.rights & NX_RIGHT_WRITE) != 0;
    out->mode = st.mode;
    out->uid = st.uid;
    out->gid = st.gid;
    return 0;
}

/* path 를 부모 디렉터리와 마지막 이름으로 나눈다. 이름공간의 뿌리("tmp:/")면 0 */
static int split_parent(const char *path, char *parent, size_t cap, const char **name)
{
    size_t root = __nl_path_root_len(path);
    const char *slash = strrchr(path, '/');
    size_t plen;

    if (slash == NULL || (size_t)(slash - path) + 1 < root || path[root] == '\0')
        return 0;
    plen = (size_t)(slash - path);
    if (plen < root)
        plen = root;
    if (plen >= cap)
        return 0;
    memcpy(parent, path, plen);
    parent[plen] = '\0';
    *name = slash + 1;
    return 1;
}

int __nl_stat_path(const char *path, struct __nl_stat *st)
{
    struct nx_stat ns;
    long h;
    char parent[NL_PATH_MAX];
    const char *name;
    unsigned long i;
    long first_err;

    /* 열지 않고 정보만 (경로의 디렉터리 순회 권한만 있으면 된다) */
    if (nx_sc(NX_SYS_STAT_PATH, (unsigned long)path, (unsigned long)&ns, 0) >= 0) {
        st->type = type_of(ns.type);
        st->size = ns.size;
        st->mode = ns.mode;
        st->uid = ns.uid;
        st->gid = ns.gid;
        st->readable = __nl_access(path, R_OK) == 0;
        st->writable = __nl_access(path, W_OK) == 0;
        return 0;
    }
    /* info:/ 처럼 NxStatPath 가 다루지 않는 곳 */
    h = raw_open(path, NL_O_READ);
    first_err = h;
    if (h >= 0) {
        int r = __nl_fstat(h, st);

        (void)nx_sc(NX_SYS_CLOSE, (unsigned long)h, 0, 0);
        return r;
    }
    /* 열 수 없으면(디렉터리, 권한) 부모의 목록에서 찾는다 */
    if (path[__nl_path_root_len(path)] == '\0') {            /* 이름공간의 뿌리 */
        st->type = NL_T_DIR;
        st->size = 0;
        st->readable = 1;
        st->writable = 0;
        return 0;
    }
    if (split_parent(path, parent, sizeof(parent), &name)) {
        for (i = 0;; i++) {
            struct nx_dirent de;
            long r = nx_sc(NX_SYS_READ_DIR, (unsigned long)parent, i, (unsigned long)&de);

            if (r < 0)
                break;
            if (strcmp(de.name, name) == 0) {
                st->type = de.type == NX_TYPE_DIR ? NL_T_DIR : NL_T_FILE;
                st->size = 0;
                st->readable = de.type == NX_TYPE_DIR;
                st->writable = 0;
                return 0;
            }
        }
    }
    return (int)ret(first_err);
}

int __nl_mkdir(const char *path, unsigned int mode)
{
    return (int)ret(nx_sc(NX_SYS_CREATE_NODE, (unsigned long)path, NX_NODE_DIR, mode & 0777U));
}

int __nl_rmdir(const char *path)
{
    struct __nl_stat st;
    long r;

    /* 노드는 파일/디렉터리 구분이 없다: 자식이 없는 노드면 무엇이든 지운다 (자식이 있으면 ENOTEMPTY) */
    if (__nl_stat_path(path, &st) < 0)
        return -1;
    r = nx_sc(NX_SYS_REMOVE_NODE, (unsigned long)path, 0, 0);
    if (r < 0) {
        struct nx_dirent de;

        /* 비어 있지 않아서 실패했는지 확인해 POSIX 오류로 */
        if (nx_sc(NX_SYS_READ_DIR, (unsigned long)path, 0, (unsigned long)&de) >= 0) {
            errno = ENOTEMPTY;
            return -1;
        }
    }
    return (int)ret(r);
}

int __nl_unlink(const char *path)
{
    struct __nl_stat st;

    /* 자식이 있는 노드(= 디렉터리로 보이는 노드)는 unlink 로 지우지 않는다 (rm -r 이 자식부터 지운다) */
    if (__nl_stat_path(path, &st) == 0 && st.type == NL_T_DIR) {
        errno = EISDIR;
        return -1;
    }
    return (int)ret(nx_sc(NX_SYS_REMOVE_NODE, (unsigned long)path, 0, 0));
}

int __nl_chmod(const char *path, unsigned int mode)
{
    return (int)ret(nx_sc(NX_SYS_SET_MODE, (unsigned long)path, mode & 0777U, 0));
}

unsigned int __nl_umask(unsigned int mask)
{
    long r = nx_sc(NX_SYS_SET_UMASK, mask & 0777U, 0, 0);

    return r < 0 ? 022U : (unsigned int)r;
}

/* 모드 비트에서 호출자에게 해당하는 비트 (소유자/그룹/기타). want: 4 = 읽기, 2 = 쓰기, 1 = 실행 */
static int mode_allows(const struct nx_stat *ns, unsigned int want)
{
    unsigned int uid;
    unsigned int gid;

    if (__nl_identity(&uid, &gid, NULL, 0) < 0)
        return 0;
    if (uid == 0)                               /* root: 실행만 x 비트가 하나는 있어야 한다 (커널과 같은 규칙) */
        return want != 1U || (ns->mode & 0111U) != 0;
    if (uid == ns->uid)
        return (ns->mode & (want << 6)) != 0;
    if (gid == ns->gid)
        return (ns->mode & (want << 3)) != 0;
    return (ns->mode & want) != 0;
}

/* 읽기/쓰기는 실제로 열어 보고 판정한다 (판정은 커널 한 곳에서: 모드 비트와 ACL 을 함께 본다) */
int __nl_access(const char *path, int amode)
{
    struct nx_stat ns;
    long h;

    if (nx_sc(NX_SYS_STAT_PATH, (unsigned long)path, (unsigned long)&ns, 0) < 0) {
        struct nx_dirent de;

        /* info:/ 등 NxStatPath 가 다루지 않는 곳: 열리거나 목록이 나오면 있는 것 (쓰기는 안 된다) */
        h = raw_open(path, NL_O_READ);
        if (h >= 0)
            (void)nx_sc(NX_SYS_CLOSE, (unsigned long)h, 0, 0);
        else if (nx_sc(NX_SYS_READ_DIR, (unsigned long)path, 0, (unsigned long)&de) < 0)
            return (int)ret(h);
        if (amode & W_OK) {
            errno = EACCES;
            return -1;
        }
        return 0;
    }
    if (amode == F_OK)
        return 0;
    if (ns.type == NX_TYPE_DIR) {
        /* 디렉터리는 열 수 없으므로 모드 비트로 판정한다 */
        if (((amode & R_OK) && !mode_allows(&ns, 4U)) || ((amode & W_OK) && !mode_allows(&ns, 2U))
            || ((amode & X_OK) && !mode_allows(&ns, 1U))) {
            errno = EACCES;
            return -1;
        }
        return 0;
    }
    if ((amode & X_OK) && !mode_allows(&ns, 1U)) {
        errno = EACCES;
        return -1;
    }
    if (amode & R_OK) {
        h = raw_open(path, NL_O_READ);
        if (h < 0)
            return (int)ret(h);
        (void)nx_sc(NX_SYS_CLOSE, (unsigned long)h, 0, 0);
    }
    if (amode & W_OK) {
        h = raw_open(path, NL_O_WRITE);
        if (h < 0)
            return (int)ret(h);
        (void)nx_sc(NX_SYS_CLOSE, (unsigned long)h, 0, 0);
    }
    return 0;
}

int __nl_readdir(const char *path, unsigned long index, char *name, size_t cap, int *type)
{
    struct nx_dirent de;
    long r = nx_sc(NX_SYS_READ_DIR, (unsigned long)path, index, (unsigned long)&de);
    size_t n;

    if (r < 0) {
        if (nst_code(r) == NST_NOT_FOUND)
            return 0;                           /* 범위를 넘었다 = 끝 */
        return (int)ret(r);
    }
    n = strlen(de.name);
    if (n >= cap)
        n = cap - 1;
    memcpy(name, de.name, n);
    name[n] = '\0';
    *type = de.type == NX_TYPE_DIR ? NL_T_DIR : NL_T_FILE;
    return 1;
}

void *__nl_sbrk(long increment)
{
    long r = nx_sc(NX_SYS_VIRTUAL_ALLOC, (unsigned long)increment, 0, 0);

    if (r < 0) {
        errno = ENOMEM;
        return NULL;
    }
    /* NxVirtualAlloc 은 "새" brk 를 돌려준다 */
    return (void *)(r - increment);
}

int __nl_remove(const char *path)
{
    return (int)ret(nx_sc(NX_SYS_REMOVE_NODE, (unsigned long)path, 0, 0));
}

int __nl_rename(const char *from, const char *to)
{
    return (int)ret(nx_sc(NX_SYS_RENAME_NODE, (unsigned long)from, (unsigned long)to, 0));
}

long __nl_clock_ns(int which)
{
    return ret(nx_sc(NX_SYS_GET_TIME, which == NL_CLOCK_REALTIME ? NX_CLOCK_REALTIME : NX_CLOCK_MONOTONIC, 0, 0));
}

int __nl_sleep_ms(unsigned long ms)
{
    while (ms > 0) {
        unsigned long chunk = ms > NX_SLEEP_MAX_MS ? NX_SLEEP_MAX_MS : ms;
        long r = nx_sc(NX_SYS_SLEEP, chunk, 0, 0);

        if (r < 0)
            return (int)ret(r);
        ms -= chunk;
    }
    return 0;
}

int __nl_identity(unsigned int *uid, unsigned int *gid, char *name, size_t cap)
{
    struct nx_identity id;
    long r = nx_sc(NX_SYS_GET_IDENTITY, (unsigned long)&id, NX_NO_TOKEN, 0);
    size_t n;

    if (r < 0)
        return (int)ret(r);
    *uid = id.uid;
    *gid = id.gid;
    if (name && cap) {
        n = strlen(id.name);
        if (n >= cap)
            n = cap - 1;
        memcpy(name, id.name, n);
        name[n] = '\0';
    }
    return 0;
}

long __nl_getpid(void)
{
    struct nx_procinfo pi;

    if (nx_sc(NX_SYS_PROCESS_INFO, NX_PID_SELF, (unsigned long)&pi, 0) < 0)
        return 0;
    return (long)pi.pid;
}

const char *__nl_tmpdir(void)
{
    return "tmp:/";             /* 재부팅하면 사라지는 RAM 파일시스템, 누구나 쓸 수 있다 */
}

/* ---------------- 프로세스 ---------------- */
long __nl_spawn(const char *path, char *const argv[], char *const envp[], int inherit)
{
    long r = nx_sc5(NX_SYS_PROCESS_CREATE, (unsigned long)path, (unsigned long)argv,
                    (unsigned long)envp, NX_NO_TOKEN, inherit ? NX_SPAWN_INHERIT : 0);

    if (r < 0 && nst_code(r) == 8U) {         /* Noverflow: 인자/환경이 NX_ARG_MAX 를 넘음 */
        errno = E2BIG;
        return -1;
    }
    return ret(r);
}

long __nl_fork(void)
{
    return ret(nx_sc(NX_SYS_PROCESS_FORK, 0, 0, 0));
}

int __nl_exec(const char *path, char *const argv[], char *const envp[])
{
    long r = nx_sc(NX_SYS_PROCESS_EXEC, (unsigned long)path, (unsigned long)argv, (unsigned long)envp);

    if (r < 0 && nst_code(r) == 8U) {          /* Noverflow: 인자/환경이 NX_ARG_MAX 를 넘음 */
        errno = E2BIG;
        return -1;
    }
    return (int)ret(r);
}

long __nl_getppid(void)
{
    struct nx_procinfo pi;

    if (nx_sc(NX_SYS_PROCESS_INFO, NX_PID_SELF, (unsigned long)&pi, 0) < 0)
        return 0;
    return (long)pi.parent_pid;
}

long __nl_proc_pid(long h)
{
    struct nx_procinfo pi;
    long r = nx_sc(NX_SYS_PROCESS_INFO, (unsigned long)h, (unsigned long)&pi, 0);

    if (r < 0)
        return ret(r);
    return (long)pi.pid;
}

/* 커널 종료 코드 -> POSIX 대기 상태 (sys/wait.h 머리말) */
static int wstatus_of(int code)
{
    if (code == NX_EXIT_KILLED)
        return 9;                               /* SIGKILL */
    if (NX_EXIT_IS_SIGNAL(code))
        return -256 - code;                     /* 신호의 기본 동작으로 끝남 (SIGINT = Ctrl-C 등) */
    if (code == NX_EXIT_FAULT)
        return 11;                              /* SIGSEGV */
    return (code & 0xFF) << 8;
}

int __nl_proc_wait(long h, int nohang, int *wstatus)
{
    int code = 0;
    long r = nx_sc(NX_SYS_PROCESS_WAIT, (unsigned long)h, nohang ? 0 : NX_WAIT_INFINITE, (unsigned long)&code);

    if (r < 0) {
        if (nohang && nst_code(r) == NST_TIMEOUT)
            return 0;
        return (int)ret(r);
    }
    *wstatus = wstatus_of(code);
    return 1;
}

int __nl_proc_wait_any(const long *hs, int n, int nohang)
{
    struct nx_pollfd p[NX_POLL_MAX];
    long r;
    int i;

    if (n <= 0 || n > (int)NX_POLL_MAX) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < n; i++) {
        p[i].handle = (unsigned long)hs[i];
        p[i].events = NX_POLL_EXIT;
        p[i].revents = 0;
    }
    r = nx_sc(NX_SYS_POLL, (unsigned long)p, (unsigned long)n, nohang ? 0 : NX_WAIT_INFINITE);
    if (r < 0)
        return (int)ret(r);
    for (i = 0; i < n; i++) {
        if (p[i].revents & NX_POLL_EXIT)
            return i;
    }
    return -2;
}

int __nl_proc_kill(long h)
{
    return (int)ret(nx_sc(NX_SYS_PROCESS_KILL, (unsigned long)h, 0, 0));
}

/* ---------------- 신호 ---------------- */
extern char __nl_sigtramp_nyx[];            /* sigtramp.s: NxSignalReturn */

#define NL_SA_SIGINFO   0x00000004UL
#define NL_SA_NODEFER   0x40000000UL
#define NL_SA_RESETHAND 0x80000000UL

int __nl_sigaction(int sig, const struct __nl_sigact *act, struct __nl_sigact *old)
{
    struct nx_sigaction k;
    struct nx_sigaction o;
    unsigned long f;
    long r;

    if (act) {
        f = (unsigned long)(unsigned int)act->flags;
        k.handler = act->handler;
        k.restorer = (unsigned long)__nl_sigtramp_nyx;
        k.mask = (uint32_t)act->mask;
        k.flags = ((f & NL_SA_SIGINFO) ? NX_SA_SIGINFO : 0U) | ((f & NL_SA_NODEFER) ? NX_SA_NODEFER : 0U) |
                  ((f & NL_SA_RESETHAND) ? NX_SA_RESETHAND : 0U);
    }
    r = nx_sc(NX_SYS_SIGNAL_ACTION, (unsigned long)sig, act ? (unsigned long)&k : 0, (unsigned long)&o);
    if (r < 0)
        return (int)ret(r);
    if (old) {
        old->handler = (unsigned long)o.handler;
        old->mask = o.mask;
        old->flags = (int)(((o.flags & NX_SA_SIGINFO) ? NL_SA_SIGINFO : 0UL) |
                           ((o.flags & NX_SA_NODEFER) ? NL_SA_NODEFER : 0UL) |
                           ((o.flags & NX_SA_RESETHAND) ? NL_SA_RESETHAND : 0UL));
    }
    return 0;
}

int __nl_sigprocmask(int how, const unsigned long *set, unsigned long *old)
{
    long r;

    if (set)
        r = nx_sc(NX_SYS_SIGNAL_MASK, (unsigned long)how, *set, 0);
    else
        r = nx_sc(NX_SYS_SIGNAL_MASK, NX_SIG_BLOCK, 0, 0);    /* 조회만 */
    if (r < 0)
        return (int)ret(r);
    if (old)
        *old = (unsigned long)r;
    return 0;
}

int __nl_sigsend(long h, int sig)
{
    return (int)ret(nx_sc(NX_SYS_SIGNAL_SEND, h < 0 ? NX_PID_SELF : (unsigned long)h, (unsigned long)sig, 0));
}

int __nl_sigpending(unsigned long *set)
{
    long r = nx_sc(NX_SYS_SIGNAL_PENDING, 0, 0, 0);

    if (r < 0)
        return (int)ret(r);
    *set = (unsigned long)r;
    return 0;
}

int __nl_sigsuspend(const unsigned long *set)
{
    return (int)ret(nx_sc(NX_SYS_SIGNAL_SUSPEND, *set, 0, 0));
}

long __nl_proc_group(long h, long pgid)
{
    return ret(nx_sc(NX_SYS_PROCESS_GROUP, h < 0 ? NX_PID_SELF : (unsigned long)h,
                     pgid < 0 ? NX_PGID_QUERY : (unsigned long)pgid, 0));
}

long __nl_tty_foreground(long h, long pgid)
{
    return ret(nx_sc(NX_SYS_DEVICE_COMMAND, (unsigned long)h, NX_DEVCMD_TTY_FOREGROUND,
                     pgid < 0 ? NX_PGID_QUERY : (unsigned long)pgid));
}

int __nl_proc_list(struct __nl_procent *out, int max)
{
    struct nx_procent buf[64];
    long r;
    int i;

    if (max < 0 || (max > 0 && out == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (max > 64)
        max = 64;
    r = nx_sc(NX_SYS_PROCESS_LIST, (unsigned long)buf, (unsigned long)max, 0);
    if (r < 0)
        return (int)ret(r);
    for (i = 0; i < max && i < (int)r; i++) {
        out[i].pid = buf[i].pid;
        out[i].ppid = buf[i].parent_pid;
        out[i].pgid = buf[i].pgid;
        out[i].uid = buf[i].uid;
        out[i].state = (int)buf[i].state;
        out[i].flags = buf[i].flags;
        out[i].cpu_ticks = buf[i].cpu_ticks;
        out[i].start_tick = buf[i].start_tick;
        out[i].mem_kib = buf[i].mem_kib;
        memcpy(out[i].name, buf[i].name, sizeof(out[i].name));
        out[i].name[sizeof(out[i].name) - 1] = '\0';
        memcpy(out[i].user, buf[i].user, sizeof(out[i].user));
        out[i].user[sizeof(out[i].user) - 1] = '\0';
    }
    return (int)r;
}

int __nl_signal_pid(unsigned long pid, int sig)
{
    long r = nx_sc(NX_SYS_SIGNAL_SEND_PID, pid, (unsigned long)sig, 0);

    if (r < 0 && nst_code(r) == 1U) {           /* NnotFound: 없는 pid */
        errno = ESRCH;
        return -1;
    }
    if (r < 0 && nst_code(r) == 4U) {           /* Npermission: 남의 것이거나 보호 프로세스 (POSIX kill 은 EPERM) */
        errno = EPERM;
        return -1;
    }
    return (int)ret(r);
}

int __nl_check_password(const char *name, const char *password)
{
    long r = nx_sc(NX_SYS_AUTHENTICATE, (unsigned long)name, (unsigned long)password, 0);

    if (r < 0) {
        (void)ret(r);
        errno = EACCES;
        return -1;
    }
    (void)nx_sc(NX_SYS_CLOSE, (unsigned long)r, 0, 0);     /* 토큰은 쓰지 않는다: 확인만 */
    return 0;
}

unsigned long __nl_timer_hz(void)
{
    struct nx_sysinfo si;

    if (nx_sc(NX_SYS_SYSINFO, (unsigned long)&si, 0, 0) < 0 || si.timer_hz == 0)
        return 100;
    return si.timer_hz;
}

void __nl_proc_release(long h)
{
    (void)nx_sc(NX_SYS_CLOSE, (unsigned long)h, 0, 0);
}

/* ---------------- 경로 규칙 ---------------- */
/* "이름:" 으로 시작하면 절대 경로 (app:/ face:/ tmp:/ info:/ ...) */
int __nl_path_is_absolute(const char *path)
{
    const char *p = path;

    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))
        p++;
    return p != path && *p == ':';
}

size_t __nl_path_root_len(const char *abs)
{
    const char *colon = strchr(abs, ':');

    if (!colon)
        return 0;
    return (size_t)(colon - abs) + (colon[1] == '/' ? 2 : 1);
}

int __nl_initial_cwd(char *buf, size_t cap)
{
    const char *pwd = getenv("PWD");

    if (pwd && __nl_path_is_absolute(pwd) && strlen(pwd) < cap) {
        strcpy(buf, pwd);
        return 0;
    }
    if (cap < 7)
        return -1;
    strcpy(buf, "face:/");                      /* 사용자 홈 */
    return 0;
}

void __nl_cwd_changed(const char *abs)
{
    (void)abs;                                  /* 커널에는 현재 디렉터리가 없다 */
}

int __nl_command_path(const char *name, char *out, size_t cap)
{
    size_t n = strlen(name);
    int has_ext = n > 4 && strcmp(name + n - 4, ".run") == 0;

    if (n + 5 + (has_ext ? 0 : 4) >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(out, "app:/");
    strcat(out, name);
    if (!has_ext)
        strcat(out, ".run");
    return 0;
}

void __nl_exit(int status)
{
    (void)nx_sc(NX_SYS_PROCESS_EXIT, (unsigned long)(unsigned int)status, 0, 0);
    for (;;)
        ;
}

/* ---------------- 서비스 채널 (svc:/) ---------------- */

long __nl_channel_create(const char *path, int public_)
{
    return ret(nx_sc(NX_SYS_CHANNEL_CREATE, (unsigned long)path, public_ ? NX_CHAN_PUBLIC : 0UL, 0));
}

long __nl_channel_open(const char *path)
{
    return ret(nx_sc(NX_SYS_OPEN, (unsigned long)path, NX_O_WRITE, 0));
}

long __nl_channel_send(long h, const void *buf, size_t len)
{
    return ret(nx_sc(NX_SYS_CHANNEL_SEND, (unsigned long)h, (unsigned long)buf, (unsigned long)len));
}

long __nl_channel_receive(long h, void *buf, size_t cap, unsigned long sender[3], long timeout_ms)
{
    struct nx_msginfo info;
    long r;

    memset(&info, 0, sizeof(info));
    r = ret(nx_sc5(NX_SYS_CHANNEL_RECEIVE, (unsigned long)h, (unsigned long)buf, (unsigned long)cap,
                   (unsigned long)&info, timeout_ms < 0 ? NX_WAIT_INFINITE : (unsigned long)timeout_ms));
    if (r >= 0 && sender != NULL) {
        sender[0] = info.sender_pid;
        sender[1] = info.sender_uid;
        sender[2] = info.sender_gid;
    }
    return r;
}
