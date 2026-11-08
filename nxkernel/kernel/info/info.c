/*
 * info.c - info:/ 가상 파일 (규칙은 info.h)
 */

#include "kernel/info/info.h"
#include "kernel/process/process.h"
#include "kernel/mm/kheap.h"
#include "kernel/mm/pfa.h"
#include "kernel/paging/paging.h"
#include "kernel/timer/pit/pit_base.h"
#include "drivers/filesystem/vfs.h"
#include "nyx_abi.h"
#include "memory.h"
#include "string.h"

#define ROI_HOSTNAME_PATH "fs:/roi/hostname"
#define ROI_TIMEZONE_PATH "fs:/roi/timezone"

typedef Nstatus (*info_gen_fn)(char *buf, usize max, usize *len);

struct info_node {
    const char *path;       /* info:/ 아래 정규화된 경로 ("" = 뿌리) */
    u32 type;               /* INFO_NODE_* */
    info_gen_fn gen;        /* 파일 내용 생성 (디렉터리는 NULL) */
};

/* ---------------- 생성 함수 ---------------- */

/* buf 에 s 를 이어 붙인다 */
static Nstatus put_str(char *buf, usize max, usize *len, const char *s)
{
    usize n = strlen(s);

    if (*len + n > max)
        return NbufferOverflow;
    memcpy(buf + *len, s, n);
    *len += n;
    return NSTATUS_OK;
}

static Nstatus put_u64(char *buf, usize max, usize *len, u64 v)
{
    char tmp[21];
    usize i = sizeof(tmp) - 1U;

    tmp[i] = '\0';
    do {
        tmp[--i] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v);
    return put_str(buf, max, len, tmp + i);
}

bool info_hostname_valid(const char *s, usize n)
{
    usize i;

    if (!s || n == 0 || n > INFO_HOSTNAME_MAX || s[0] == '-')
        return false;
    for (i = 0; i < n; i++) {
        char c = s[i];

        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

/* roi:/timezone 형식 검사 (info.h) */
bool info_timezone_valid(const char *s, usize n)
{
    usize i;

    if (!s || n == 0 || n > INFO_TIMEZONE_MAX || !((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')))
        return false;
    for (i = 0; i < n; i++) {
        char c = s[i];

        if (c == '/') {
            if (i + 1U == n || s[i + 1U] == '/')
                return false;               /* 빈 구성요소 */
            continue;
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '+'))
            return false;
    }
    return true;
}

/*
 * roi:/<이름> 은 원본, info:/<이름> 은 누구나 보는 창이다. 원본 파일의 모드와 상관없이 보여야 하므로
 * 커널 신원(uid 0)으로 읽는다: nyfs 는 current_process 의 uid/gid 로 권한을 검사하고, 시스템 콜 안은
 * 인터럽트가 꺼져 있고 커널은 비선점이며 RAM 파일시스템 읽기는 잠들지 않으므로 그 사이에 이 프로세스의
 * 신원으로 다른 일이 일어나지 않는다.
 * raw 에 최대 cap 바이트를 읽고 끝의 줄바꿈 하나를 뗀 길이를 돌려준다 (없거나 못 읽으면 0).
 * cap 을 꽉 채우면 값이 너무 긴 것이다 (호출자가 cap 을 최대 길이 + 2 로 잡아 알아본다).
 */
static usize read_roi_value(const char *path, char *raw, usize cap)
{
    process_t *self = current_process;
    u32 su = 0;
    u32 sg = 0;
    usize n = 0;
    handle_t h;
    Nstatus status;

    if (self) {
        su = self->uid;
        sg = self->gid;
        self->uid = 0;
        self->gid = 0;
    }
    status = vfs_open(path, NX_O_READ, &h);
    if (!NSTATUS_IS_ERR(status)) {
        while (n < cap) {
            usize got = 0;

            if (NSTATUS_IS_ERR(vfs_read(&h, raw + n, cap - n, &got)) || got == 0)
                break;
            n += got;
        }
        (void)vfs_close(&h);
    }
    if (self) {
        self->uid = su;
        self->gid = sg;
    }

    if (NSTATUS_IS_ERR(status))
        return 0;
    if (n > 0 && n < cap && raw[n - 1] == '\n')
        n--;                                /* 끝의 줄바꿈 하나만 */
    return n;
}

/* roi:/<이름> 의 값을 검사해 내보낸다. 없거나 형식이 틀리면 기본값 */
static Nstatus gen_roi_value(char *buf, usize max, usize *len, const char *path, usize value_max,
                             bool (*valid)(const char *, usize), const char *fallback)
{
    char raw[INFO_ROI_VALUE_MAX + 2U];      /* 최대 길이 + '\n' + 1 (너무 긴 값을 알아보려고) */
    usize cap = value_max + 2U;
    usize n;
    Nstatus status;

    if (cap > sizeof(raw))
        cap = sizeof(raw);
    n = read_roi_value(path, raw, cap);
    if (n == 0 || n >= cap - 1U || !valid(raw, n))
        status = put_str(buf, max, len, fallback);
    else {
        raw[n] = '\0';
        status = put_str(buf, max, len, raw);
    }
    return NSTATUS_IS_ERR(status) ? status : put_str(buf, max, len, "\n");
}

static Nstatus gen_hostname(char *buf, usize max, usize *len)
{
    return gen_roi_value(buf, max, len, ROI_HOSTNAME_PATH, INFO_HOSTNAME_MAX, info_hostname_valid,
                         INFO_DEFAULT_HOSTNAME);
}

/* 이름만 보여 준다: 시각(NX_CLOCK_REALTIME)은 늘 UTC 이고 오프셋/서머타임은 적용하지 않는다 */
static Nstatus gen_timezone(char *buf, usize max, usize *len)
{
    return gen_roi_value(buf, max, len, ROI_TIMEZONE_PATH, INFO_TIMEZONE_MAX, info_timezone_valid,
                         INFO_DEFAULT_TIMEZONE);
}

/* UTC 오프셋 "+HH:MM". 아직 시간대 계산이 없으므로 늘 +00:00 (gen_timezone 참고) */
static Nstatus gen_utc_offset(char *buf, usize max, usize *len)
{
    return put_str(buf, max, len, INFO_UTC_OFFSET "\n");
}

static Nstatus gen_version(char *buf, usize max, usize *len)
{
    return put_str(buf, max, len, NYX_OS_VERSION "\n");
}

static Nstatus gen_abi(char *buf, usize max, usize *len)
{
    Nstatus status = put_u64(buf, max, len, NX_ABI_MAJOR);

    if (!NSTATUS_IS_ERR(status))
        status = put_str(buf, max, len, ".");
    if (!NSTATUS_IS_ERR(status))
        status = put_u64(buf, max, len, NX_ABI_MINOR);
    if (!NSTATUS_IS_ERR(status))
        status = put_str(buf, max, len, "\n");
    return status;
}

static Nstatus gen_uptime_ms(char *buf, usize max, usize *len)
{
    Nstatus status = put_u64(buf, max, len, timer_get_ns() / 1000000UL);

    return NSTATUS_IS_ERR(status) ? status : put_str(buf, max, len, "\n");
}

static Nstatus gen_mem_total(char *buf, usize max, usize *len)
{
    Nstatus status = put_u64(buf, max, len, pfa_total_count() * (PAGE_SIZE / 1024U));

    return NSTATUS_IS_ERR(status) ? status : put_str(buf, max, len, "\n");
}

static Nstatus gen_mem_free(char *buf, usize max, usize *len)
{
    Nstatus status = put_u64(buf, max, len, pfa_free_count() * (PAGE_SIZE / 1024U));

    return NSTATUS_IS_ERR(status) ? status : put_str(buf, max, len, "\n");
}

/* ---------------- 표 ---------------- */

/* 디렉터리는 자기 항목보다 앞에 둔다. 나열 순서 = 표 순서 */
static const struct info_node g_nodes[] = {
    { "",                 INFO_NODE_DIR,  nNULL },
    { "hostname",         INFO_NODE_FILE, gen_hostname },
    { "timezone",         INFO_NODE_FILE, gen_timezone },
    { "utc_offset",       INFO_NODE_FILE, gen_utc_offset },
    { "version",          INFO_NODE_FILE, gen_version },
    { "abi",              INFO_NODE_FILE, gen_abi },
    { "uptime_ms",        INFO_NODE_FILE, gen_uptime_ms },
    { "memory",           INFO_NODE_DIR,  nNULL },
    { "memory/total_kib", INFO_NODE_FILE, gen_mem_total },
    { "memory/free_kib",  INFO_NODE_FILE, gen_mem_free },
};

#define INFO_NODE_COUNT (sizeof(g_nodes) / sizeof(g_nodes[0]))

static const struct info_node *find(const char *name)
{
    usize i;

    for (i = 0; i < INFO_NODE_COUNT; i++) {
        if (strcmp(g_nodes[i].path, name) == 0)
            return &g_nodes[i];
    }
    return nNULL;
}

Nstatus info_open(const char *name, u32 *out_type, char **out_buf, usize *out_len)
{
    const struct info_node *node;
    char *buf;
    usize len = 0;
    Nstatus status;

    if (!name || !out_type || !out_buf || !out_len)
        return NinvalidArg;
    node = find(name);
    if (!node)
        return NnotFound;

    *out_type = node->type;
    *out_buf = nNULL;
    *out_len = 0;
    if (node->type == INFO_NODE_DIR)
        return NSTATUS_OK;

    buf = (char *)kmalloc(INFO_VALUE_MAX);
    if (!buf)
        return NoutOfMemory;
    status = node->gen(buf, INFO_VALUE_MAX, &len);
    if (NSTATUS_IS_ERR(status)) {
        kfree(buf);
        return status;
    }
    *out_buf = buf;
    *out_len = len;
    return NSTATUS_OK;
}

Nstatus info_readdir(const char *name, u64 index, char *out_name, usize out_max, u32 *out_type)
{
    const struct info_node *dir;
    usize dlen;
    u64 seen = 0;
    usize i;

    if (!name || !out_name || out_max == 0 || !out_type)
        return NinvalidArg;
    dir = find(name);
    if (!dir)
        return NnotFound;
    if (dir->type != INFO_NODE_DIR)
        return NinvalidArg;                 /* 파일은 나열할 수 없다 */
    dlen = strlen(name);

    for (i = 0; i < INFO_NODE_COUNT; i++) {
        const char *p = g_nodes[i].path;
        const char *child;

        if (p[0] == '\0')
            continue;
        if (dlen) {
            if (strncmp(p, name, dlen) != 0 || p[dlen] != '/')
                continue;
            child = p + dlen + 1;
        } else {
            child = p;
        }
        if (strchr(child, '/'))
            continue;                       /* 바로 아래 항목만 */
        if (seen++ != index)
            continue;
        if (strlen(child) + 1U > out_max)
            return NbufferOverflow;
        memcpy(out_name, child, strlen(child) + 1U);
        *out_type = g_nodes[i].type;
        return NSTATUS_OK;
    }
    return NnotFound;
}
