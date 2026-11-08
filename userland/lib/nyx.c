/*
 * nyx.c - 유저 프로그램 공용 런타임 구현 (설계는 nyx.h 참고)
 */
#include "nyx.h"

long nx_syscall(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2,
                unsigned long a3, unsigned long a4)
{
    unsigned long ret;
    register unsigned long r10_ __asm__("r10") = a3;
    register unsigned long r8_  __asm__("r8")  = a4;
    register unsigned long r9_  __asm__("r9")  = 0;

    /* `syscall` 명령 경로 (rcx, r11 파괴) */
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10_), "r"(r8_), "r"(r9_)
        : "rcx", "r11", "memory"
    );
    return (long)ret;
}

#define SC0(n)             nx_syscall((n), 0, 0, 0, 0, 0)
#define SC1(n, a)          nx_syscall((n), (unsigned long)(a), 0, 0, 0, 0)
#define SC2(n, a, b)       nx_syscall((n), (unsigned long)(a), (unsigned long)(b), 0, 0, 0)
#define SC3(n, a, b, c)    nx_syscall((n), (unsigned long)(a), (unsigned long)(b), (unsigned long)(c), 0, 0)

long nx_open(const char *path, unsigned long flags) { return SC2(NX_SYS_OPEN, path, flags); }
long nx_close(long h) { return SC1(NX_SYS_CLOSE, h); }
long nx_read(long h, void *buf, unsigned long len) { return SC3(NX_SYS_READ, h, buf, len); }
long nx_write(long h, const void *buf, unsigned long len) { return SC3(NX_SYS_WRITE, h, buf, len); }
long nx_seek(long h, long offset, unsigned long whence) { return SC3(NX_SYS_SEEK, h, offset, whence); }
long nx_stat(long h, struct nx_stat *out) { return SC2(NX_SYS_STAT, h, out); }
long nx_dup(long h, unsigned long rights) { return SC2(NX_SYS_DUPLICATE_HANDLE, h, rights); }
long nx_sleep(unsigned long ms) { return SC1(NX_SYS_SLEEP, ms); }
long nx_identity(struct nx_identity *out) { return SC2(NX_SYS_GET_IDENTITY, out, NX_NO_TOKEN); }
long nx_token_identity(long token, struct nx_identity *out) { return SC2(NX_SYS_GET_IDENTITY, out, token); }
long nx_authenticate(const char *name, const char *password) { return SC2(NX_SYS_AUTHENTICATE, name, password); }
long nx_kill(long h) { return SC1(NX_SYS_PROCESS_KILL, h); }
long nx_process_info(long h, struct nx_procinfo *out) { return SC2(NX_SYS_PROCESS_INFO, h, out); }
long nx_poll(struct nx_pollfd *fds, unsigned long n, unsigned long timeout_ms) { return SC3(NX_SYS_POLL, fds, n, timeout_ms); }
long nx_channel_create(const char *path, unsigned long flags) { return SC2(NX_SYS_CHANNEL_CREATE, path, flags); }
long nx_channel_send(long h, const void *buf, unsigned long len) { return SC3(NX_SYS_CHANNEL_SEND, h, buf, len); }
long nx_create_node(const char *path, unsigned long type, unsigned long mode) { return SC3(NX_SYS_CREATE_NODE, path, type, mode); }
long nx_remove_node(const char *path) { return SC1(NX_SYS_REMOVE_NODE, path); }
long nx_rename_node(const char *from, const char *to) { return SC2(NX_SYS_RENAME_NODE, from, to); }
long nx_read_dir(const char *path, unsigned long index, struct nx_dirent *out) { return SC3(NX_SYS_READ_DIR, path, index, out); }
long nx_device_command(long h, unsigned long cmd, unsigned long arg) { return SC3(NX_SYS_DEVICE_COMMAND, h, cmd, arg); }
long nx_device_map(long h, unsigned long *out_addr) { return SC2(NX_SYS_DEVICE_MAP_MMIO, h, out_addr); }
long nx_set_echo(int on) { return SC3(NX_SYS_DEVICE_COMMAND, NX_HANDLE_STDIN, NX_DEVCMD_TTY_ECHO, on ? 1 : 0); }
long nx_power_off(void) { return SC0(NX_SYS_POWER_OFF); }

long nx_spawn(const char *path, const char *const *argv, const char *const *envp, long token)
{
    return nx_syscall(NX_SYS_PROCESS_CREATE, (unsigned long)path, (unsigned long)argv,
                      (unsigned long)envp, (unsigned long)token, 0);
}

long nx_wait(long h, unsigned long timeout_ms, int *code)
{
    return SC3(NX_SYS_PROCESS_WAIT, h, timeout_ms, code);
}

long nx_channel_receive(long h, void *buf, unsigned long len, struct nx_msginfo *info, unsigned long timeout_ms)
{
    return nx_syscall(NX_SYS_CHANNEL_RECEIVE, (unsigned long)h, (unsigned long)buf, len,
                      (unsigned long)info, timeout_ms);
}

void nx_exit(int code)
{
    (void)SC1(NX_SYS_PROCESS_EXIT, (unsigned long)(unsigned int)code);
    for (;;)
        ;
}

unsigned long nx_strlen(const char *s)
{
    unsigned long n = 0;

    while (s[n])
        n++;
    return n;
}

int nx_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int nx_strncmp(const char *a, const char *b, unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0')
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
    }
    return 0;
}

void *nx_memset(void *p, int v, unsigned long n)
{
    unsigned char *d = (unsigned char *)p;

    while (n--)
        *d++ = (unsigned char)v;
    return p;
}

void *nx_memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while (n--)
        *d++ = *s++;
    return dst;
}

int nx_memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    unsigned long i;

    for (i = 0; i < n; i++) {
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    }
    return 0;
}

void nx_wipe(void *p, unsigned long n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;

    while (n--)
        *v++ = 0;
}

void nx_puts(const char *s)
{
    (void)nx_write((long)NX_HANDLE_STDOUT, s, nx_strlen(s));
}

void nx_putu(unsigned long v)
{
    char buf[24];
    int i = (int)sizeof(buf) - 1;

    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + (int)(v % 10UL));
        v /= 10UL;
    } while (v && i > 0);
    nx_puts(&buf[i]);
}

void nx_puti(long v)
{
    if (v < 0) {
        nx_puts("-");
        nx_putu((unsigned long)(-(v + 1)) + 1UL);
    } else {
        nx_putu((unsigned long)v);
    }
}

long nx_readline(char *buf, unsigned long max)
{
    char tmp[256];
    unsigned long len = 0;

    if (max == 0)
        return NX_E_INVALID_ARG;

    for (;;) {
        long r = nx_read((long)NX_HANDLE_STDIN, tmp, sizeof(tmp));
        long i;

        if (r < 0)
            return r;
        if (r == 0)
            return len ? (long)len : -1;    /* EOF */
        for (i = 0; i < r; i++) {
            if (tmp[i] == '\n') {
                buf[len] = '\0';
                nx_wipe(tmp, sizeof(tmp));
                return (long)len;
            }
            if (len + 1 < max)
                buf[len++] = tmp[i];
        }
        /* 개행 없이 끝난 조각(Ctrl-D 로 넘긴 줄): 그대로 한 줄로 취급 */
        buf[len] = '\0';
        nx_wipe(tmp, sizeof(tmp));
        return (long)len;
    }
}

long nx_read_value(const char *path, char *buf, unsigned long max)
{
    long h;
    long total = 0;
    long n;
    char extra;

    if (max == 0)
        return NX_E_INVALID_ARG;
    h = nx_open(path, NX_O_READ);
    if (h < 0)
        return h;
    while ((unsigned long)total < max - 1) {
        n = nx_read(h, buf + total, max - 1 - (unsigned long)total);
        if (n < 0) {
            (void)nx_close(h);
            return n;
        }
        if (n == 0)
            break;
        total += n;
    }
    n = ((unsigned long)total == max - 1) ? nx_read(h, &extra, 1) : 0;
    (void)nx_close(h);
    if (n > 0)
        return NX_E_BUF_OVERFLOW;
    if (total > 0 && buf[total - 1] == '\n')
        total--;
    buf[total] = '\0';
    return total;
}
