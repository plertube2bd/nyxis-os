/*
 * nx64_sys.c - nx64api 의 시스템 부분 (규칙은 nx64.h): info:/, 신원, 설정, svc:/ 서비스, 전원 끄기 요청
 *
 * 시스템 콜은 nlibc 플랫폼 계층(nl_internal.h)으로만 부른다: Nyxis 는 nxsys.c, 호스트 테스트는
 * sys_linux.c (svc:/ 와 info:/ 가 없어 -1 을 돌려준다).
 */
#include <nx64.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "nl_internal.h"
#include "nyx_abi.h"

/* ---------------- info:/ ---------------- */

long nx64_info(const char *name, char *buf, size_t cap)
{
    char path[128];
    size_t len = 0;
    ssize_t n;
    int fd;

    if (name == NULL || buf == NULL || cap == 0 || strstr(name, "..") != NULL ||
        strlen(name) + 7 >= sizeof(path)) {
        errno = EINVAL;
        return -1;
    }
    sprintf(path, "info:/%s", name);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    while (len + 1 < cap && (n = read(fd, buf + len, cap - 1 - len)) > 0)
        len += (size_t)n;
    close(fd);
    buf[len] = '\0';
    if (len > 0 && buf[len - 1] == '\n')
        buf[--len] = '\0';
    return (long)len;
}

int nx64_info_ulong(const char *name, unsigned long *out)
{
    char buf[32];
    char *end;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (nx64_info(name, buf, sizeof(buf)) <= 0)
        return -1;
    *out = strtoul(buf, &end, 10);
    if (*end != '\0') {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int nx64_memory(struct nx64_memory *out)
{
    unsigned long total = 0;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (nx64_info_ulong("memory/free_kib", &out->free_kib) < 0 ||
        nx64_info_ulong("memory/total_kib", &total) < 0)
        return -1;
    if (nx64_info_ulong("memory/ram_kib", &out->ram_kib) < 0)
        out->ram_kib = total;           /* 옛 커널 */
    if (nx64_info_ulong("memory/reserved_kib", &out->reserved_kib) < 0)
        out->reserved_kib = out->ram_kib - total;
    out->used_kib = out->ram_kib > out->free_kib ? out->ram_kib - out->free_kib : 0;
    out->managed_kib = total;
    out->alloc_kib = total > out->free_kib ? total - out->free_kib : 0;
    return 0;
}

unsigned long nx64_uptime_ms(void)
{
    unsigned long v = 0;

    return nx64_info_ulong("uptime_ms", &v) == 0 ? v : 0;
}

/* ---------------- 신원 / 설정 ---------------- */

int nx64_whoami(struct nx64_identity *out)
{
    unsigned int uid;
    unsigned int gid;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (__nl_identity(&uid, &gid, out->name, sizeof(out->name)) < 0)
        return -1;
    out->uid = uid;
    out->gid = gid;
    if (out->name[0] == '\0') {        /* 이름을 모르는 플랫폼 (호스트): getlogin 의 환경 변수 */
        const char *n = getlogin();

        strncpy(out->name, n != NULL ? n : "?", sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';
    }
    return 0;
}

long nx64_setting_get(const char *app, const char *key, char *buf, size_t cap)
{
    return nyx_setting_get(app, key, buf, cap);
}

int nx64_setting_set(const char *app, const char *key, const char *value)
{
    return nyx_setting_set(app, key, value);
}

int nx64_setting_reset(const char *app, const char *key)
{
    return nyx_setting_reset(app, key);
}

int nx64_setting_path(int where, const char *app, const char *key, char *buf, size_t cap)
{
    return nyx_setting_path(where, app, key, buf, cap);
}

/* ---------------- 서비스 ---------------- */

/* "svc:/..." 는 그대로, 아니면 "svc:/<내 이름>/<name>" */
static int service_path(const char *name, char *out, size_t cap)
{
    struct nx64_identity me;

    if (name == NULL || *name == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (strncmp(name, "svc:/", 5) == 0) {
        if (strlen(name) >= cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(out, name);
        return 0;
    }
    if (strchr(name, '/') != NULL || nx64_whoami(&me) < 0) {
        if (errno == 0 || strchr(name, '/') != NULL)
            errno = EINVAL;
        return -1;
    }
    if (strlen(me.name) + strlen(name) + 7 >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    sprintf(out, "svc:/%s/%s", me.name, name);
    return 0;
}

long nx64_service_create(const char *name, int public_)
{
    char path[160];

    if (service_path(name, path, sizeof(path)) < 0)
        return -1;
    return __nl_channel_create(path, public_);
}

long nx64_service_receive(long h, void *buf, size_t cap, struct nx64_sender *from, int timeout_ms)
{
    unsigned long sender[3];
    long r = __nl_channel_receive(h, buf, cap, sender, timeout_ms);

    if (r >= 0 && from != NULL) {
        from->pid = sender[0];
        from->uid = sender[1];
        from->gid = sender[2];
    }
    return r;
}

int nx64_service_send(const char *name, const void *buf, size_t len)
{
    char path[160];
    long h;
    long r;
    int saved;

    if (service_path(name, path, sizeof(path)) < 0)
        return -1;
    h = __nl_channel_open(path);
    if (h < 0)
        return -1;
    r = __nl_channel_send(h, buf, len);
    saved = errno;
    (void)__nl_close(h);
    errno = saved;
    return r < 0 ? -1 : 0;
}

int nx64_service_send_fd(const char *name, const void *buf, size_t len, int fd)
{
    char path[160];
    long h;
    long a = __nl_fd_to_handle(fd);
    int r;
    int saved;

    if (a < 0 || service_path(name, path, sizeof(path)) < 0)
        return -1;
    h = __nl_channel_open(path);
    if (h < 0)
        return -1;
    r = __nl_channel_send_handle(h, buf, len, a);
    saved = errno;
    (void)__nl_close(h);
    errno = saved;
    return r < 0 ? -1 : 0;
}

long nx64_service_receive_fd(long h, void *buf, size_t cap, struct nx64_sender *from, int timeout_ms, int *out_fd)
{
    unsigned long sender[3];
    long r = __nl_channel_receive_handle(h, buf, cap, sender, timeout_ms, out_fd);

    if (r >= 0 && from != NULL) {
        from->pid = sender[0];
        from->uid = sender[1];
        from->gid = sender[2];
    }
    return r;
}

int nx64_service_fd(long h)
{
    int fd = __nl_adopt_handle(h, NL_O_READ);

    if (fd >= 0)
        (void)__nl_handle_cloexec(h, 1);
    return fd;
}

int nx64_service_connect(const char *name)
{
    char path[160];
    long h;
    int fd;

    if (service_path(name, path, sizeof(path)) < 0)
        return -1;
    h = __nl_channel_open(path);
    if (h < 0)
        return -1;
    fd = __nl_adopt_handle(h, NL_O_WRITE);
    if (fd < 0)
        (void)__nl_close(h);
    return fd;
}

int nx64_shm_create(size_t size)
{
    long h = __nl_shm_create(size, 1);

    return h < 0 ? -1 : __nl_handle_to_fd(h);
}

void *nx64_shm_map(int fd)
{
    long h = __nl_fd_to_handle(fd);

    return h < 0 ? NULL : __nl_shm_map(h);
}

int nx64_request_poweroff(void)
{
    return nx64_service_send("svc:/sys/init", "poweroff", 8);
}

/* ---------------- 프로세스 ---------------- */

int nx64_processes(struct nx64_proc *out, int max)
{
    struct __nl_procent buf[64];
    int n;
    int i;

    if (max < 0 || (max > 0 && out == NULL)) {
        errno = EINVAL;
        return -1;
    }
    n = __nl_proc_list(buf, 64);
    if (n < 0)
        return -1;
    for (i = 0; i < max && i < n && i < 64; i++) {
        out[i].pid = buf[i].pid;
        out[i].ppid = buf[i].ppid;
        out[i].pgid = buf[i].pgid;
        out[i].uid = buf[i].uid;
        out[i].state = buf[i].state;
        out[i].flags = buf[i].flags;
        out[i].cpu_ticks = buf[i].cpu_ticks;
        out[i].start_tick = buf[i].start_tick;
        out[i].mem_kib = buf[i].mem_kib;
        memcpy(out[i].name, buf[i].name, sizeof(out[i].name));
        memcpy(out[i].user, buf[i].user, sizeof(out[i].user));
    }
    return n;
}

int nx64_signal_pid(unsigned long pid, int sig)
{
    return __nl_signal_pid(pid, sig);
}

unsigned long nx64_find_process(const char *name)
{
    struct __nl_procent buf[64];
    unsigned long self = (unsigned long)__nl_getpid();
    int n;
    int i;

    if (name == NULL)
        return 0;
    n = __nl_proc_list(buf, 64);
    for (i = 0; i < n && i < 64; i++) {
        if (buf[i].pid != self && (buf[i].flags & 1U) && strcmp(buf[i].name, name) == 0)
            return buf[i].pid;
    }
    return 0;
}

int nx64_check_password(const char *password)
{
    struct nx64_identity me;

    if (password == NULL || nx64_whoami(&me) < 0) {
        errno = EINVAL;
        return -1;
    }
    return __nl_check_password(me.name, password);
}

long nx64_pty_foreground(int master_fd)
{
    long h = __nl_fd_to_handle(master_fd);

    if (h < 0)
        return -1;
    return __nl_devcmd(h, NX_DEVCMD_TTY_FOREGROUND, NX_PGID_QUERY);
}

unsigned long nx64_timer_hz(void)
{
    return __nl_timer_hz();
}

/* ---------------- 화면 (dev:/console) ---------------- */

int nx64_display_open(struct nx64_display *d)
{
    struct nx_devinfo info;
    struct nx_fontinfo fi;
    long h;
    int fd;
    size_t need;

    if (d == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(d, 0, sizeof(*d));
    d->fd = -1;
    fd = open("dev:/console", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    h = __nl_fd_to_handle(fd);
    if (h < 0 || __nl_devcmd(h, NX_DEVCMD_INFO, (unsigned long)&info) < 0)
        goto fail;
    if (info.bpp != 32 || info.width == 0 || info.height == 0 || info.pitch < info.width * 4U) {
        errno = ENOTSUP;
        goto fail;
    }
    d->fb = (unsigned int *)__nl_map_mmio(h);
    if (d->fb == NULL)
        goto fail;
    d->fd = fd;
    d->width = (int)info.width;
    d->height = (int)info.height;
    d->stride = (int)(info.pitch / 4U);
    d->bgr = info.format == NX_FB_FORMAT_XBGR;

    memset(&fi, 0, sizeof(fi));
    if (__nl_devcmd(h, NX_DEVCMD_CONSOLE_FONT, (unsigned long)&fi) < 0)
        goto fail;
    need = (size_t)fi.count * fi.cell_w * fi.cell_h;
    d->font = (unsigned char *)malloc(need);
    if (d->font == NULL) {
        errno = ENOMEM;
        goto fail;
    }
    fi.buf = (unsigned long)d->font;
    fi.cap = need;
    if (__nl_devcmd(h, NX_DEVCMD_CONSOLE_FONT, (unsigned long)&fi) < 0)
        goto fail;
    d->cell_w = (int)fi.cell_w;
    d->cell_h = (int)fi.cell_h;
    d->font_first = (int)fi.first;
    d->font_count = (int)fi.count;
    return 0;

fail:
    {
        int e = errno;

        free(d->font);
        (void)close(fd);
        memset(d, 0, sizeof(*d));
        d->fd = -1;
        errno = e;
    }
    return -1;
}

void nx64_display_close(struct nx64_display *d)
{
    if (d == NULL || d->fd < 0)
        return;
    (void)close(d->fd);                 /* 커널이 매핑을 풀고 화면을 콘솔로 돌려준다 */
    free(d->font);
    memset(d, 0, sizeof(*d));
    d->fd = -1;
}

unsigned int nx64_display_rgb(const struct nx64_display *d, unsigned int rgb)
{
    rgb &= 0xFFFFFFU;
    if (d != NULL && d->bgr)
        return ((rgb & 0xFFU) << 16) | (rgb & 0xFF00U) | ((rgb >> 16) & 0xFFU);
    return rgb;
}

int nx64_display_keys(struct nx64_display *d, struct nx64_keyevent *ev, int max, int timeout_ms)
{
    long h;
    long n;
    int r;

    if (d == NULL || d->fd < 0 || ev == NULL || max <= 0) {
        errno = EINVAL;
        return -1;
    }
    h = __nl_fd_to_handle(d->fd);
    if (h < 0)
        return -1;
    r = __nl_wait_input(h, timeout_ms);
    if (r <= 0)
        return r;
    n = __nl_read(h, ev, (size_t)max * sizeof(*ev));
    if (n < 0)
        return -1;
    return (int)(n / (long)sizeof(*ev));
}
