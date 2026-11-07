/*
 * auth.c - 사용자 DB 로드와 비밀번호 검증 (설계는 auth.h 참고)
 */

#include "kernel/security/auth.h"
#include "kernel/crypto/sha256.h"
#include "kernel/mm/kheap.h"
#include "drivers/filesystem/vfs.h"
#include "console/outputs/printk.h"
#include "memory.h"
#include "string.h"

#define AUTH_SALT_MAX 32U

typedef struct {
    auth_identity_t id;
    u32 iterations;
    u32 salt_len;
    u8  salt[AUTH_SALT_MAX];
    u8  hash[SHA256_DIGEST_SIZE];
} auth_user_t;

static auth_user_t g_users[AUTH_MAX_USERS];
static u32 g_user_count = 0;

static bool valid_name(const char *s, usize n)
{
    usize i;

    if (n == 0 || n >= NX_USER_NAME_MAX)
        return false;
    for (i = 0; i < n; i++) {
        char c = s[i];

        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

static bool parse_u32(const char *s, usize n, u32 *out)
{
    u64 v = 0;
    usize i;

    if (n == 0 || n > 10)
        return false;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10U + (u64)(s[i] - '0');
    }
    if (v > 0xFFFFFFFFUL)
        return false;
    *out = (u32)v;
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex(const char *s, usize n, u8 *out, usize out_max, usize *out_len)
{
    usize i;

    if (n == 0 || (n & 1U) || n / 2U > out_max)
        return false;
    for (i = 0; i < n; i += 2) {
        int hi = hexval(s[i]);
        int lo = hexval(s[i + 1]);

        if (hi < 0 || lo < 0)
            return false;
        out[i / 2U] = (u8)((hi << 4) | lo);
    }
    *out_len = n / 2U;
    return true;
}

/* line[0..len) 한 줄을 파싱해 u 에 채운다 */
static bool parse_line(const char *line, usize len, auth_user_t *u)
{
    const char *f[6];
    usize fl[6];
    u32 nf = 0;
    usize i;
    usize start = 0;
    usize hash_len = 0;
    usize salt_len = 0;

    for (i = 0; i <= len; i++) {
        if (i == len || line[i] == ':') {
            if (nf >= 6)
                return false;
            f[nf] = line + start;
            fl[nf] = i - start;
            nf++;
            start = i + 1;
        }
    }
    if (nf != 6)
        return false;

    memset(u, 0, sizeof(*u));
    if (!valid_name(f[0], fl[0]))
        return false;
    memcpy(u->id.name, f[0], fl[0]);
    if (!parse_u32(f[1], fl[1], &u->id.uid) || !parse_u32(f[2], fl[2], &u->id.gid) ||
        !parse_u32(f[3], fl[3], &u->iterations))
        return false;
    if (u->iterations < AUTH_MIN_ITERATIONS || u->iterations > AUTH_MAX_ITERATIONS)
        return false;
    if (!parse_hex(f[4], fl[4], u->salt, sizeof(u->salt), &salt_len) || salt_len < 8U)
        return false;
    u->salt_len = (u32)salt_len;
    if (!parse_hex(f[5], fl[5], u->hash, sizeof(u->hash), &hash_len) || hash_len != SHA256_DIGEST_SIZE)
        return false;
    return true;
}

Nstatus auth_load_users(const char *path, u32 *out_count)
{
    handle_t h;
    char *buf;
    usize total = 0;
    usize i;
    usize line_start = 0;
    u32 line_no = 0;
    Nstatus status;

    g_user_count = 0;
    if (out_count)
        *out_count = 0;

    buf = (char *)kpage_alloc();
    if (!buf)
        return NoutOfMemory;

    status = vfs_open(path, 1U, &h);
    if (NSTATUS_IS_ERR(status)) {
        kpage_free(buf);
        return status;
    }
    for (;;) {
        usize got = 0;

        if (total >= AUTH_DB_MAX) {
            /* 남은 내용이 있으면 너무 크다 */
            char extra;

            status = vfs_read(&h, &extra, 1, &got);
            if (!NSTATUS_IS_ERR(status) && got > 0)
                status = NbufferOverflow;
            break;
        }
        status = vfs_read(&h, buf + total, AUTH_DB_MAX - total, &got);
        if (NSTATUS_IS_ERR(status) || got == 0)
            break;
        total += got;
    }
    (void)vfs_close(&h);
    if (NSTATUS_IS_ERR(status)) {
        crypto_wipe(buf, 4096);
        kpage_free(buf);
        return status;
    }
    buf[total] = '\0';

    for (i = 0; i <= total; i++) {
        if (i == total || buf[i] == '\n') {
            usize len = i - line_start;
            const char *line = buf + line_start;

            line_no++;
            if (len > 0 && line[len - 1] == '\r')
                len--;
            if (len > 0 && line[0] != '#') {
                if (g_user_count >= AUTH_MAX_USERS) {
                    printk("auth: too many users, ignoring line %u\n", line_no);
                } else if (parse_line(line, len, &g_users[g_user_count])) {
                    g_user_count++;
                } else {
                    printk("auth: malformed user entry at line %u ignored\n", line_no);
                }
            }
            line_start = i + 1;
        }
    }

    crypto_wipe(buf, 4096);
    kpage_free(buf);
    if (out_count)
        *out_count = g_user_count;
    return NSTATUS_OK;
}

Nstatus auth_lookup(const char *name, auth_identity_t *out)
{
    u32 i;

    if (!name || !out)
        return NinvalidArg;
    for (i = 0; i < g_user_count; i++) {
        if (strcmp(g_users[i].id.name, name) == 0) {
            *out = g_users[i].id;
            return NSTATUS_OK;
        }
    }
    return NnotFound;
}

Nstatus auth_verify(const char *name, const char *password, auth_identity_t *out)
{
    static const u8 dummy_salt[16] = { 0 };
    const auth_user_t *u = nNULL;
    u8 derived[SHA256_DIGEST_SIZE];
    usize pw_len;
    bool ok;
    u32 i;

    if (!name || !password || !out)
        return NinvalidArg;
    pw_len = strnlen(password, AUTH_PASSWORD_MAX + 1U);
    if (pw_len > AUTH_PASSWORD_MAX)
        return NinvalidArg;

    for (i = 0; i < g_user_count; i++) {
        if (strcmp(g_users[i].id.name, name) == 0) {
            u = &g_users[i];
            break;
        }
    }

    if (u) {
        pbkdf2_hmac_sha256(password, pw_len, u->salt, u->salt_len, u->iterations, derived, sizeof(derived));
        ok = crypto_equal_ct(derived, u->hash, sizeof(derived));
    } else {
        /* 없는 사용자: 같은 일을 하고 버린다 (사용자 존재 여부가 시간으로 새지 않게) */
        u32 iters = g_user_count ? g_users[0].iterations : 20000U;

        pbkdf2_hmac_sha256(password, pw_len, dummy_salt, sizeof(dummy_salt), iters, derived, sizeof(derived));
        ok = false;
    }
    crypto_wipe(derived, sizeof(derived));

    if (!ok)
        return Nauthentication;
    *out = u->id;
    return NSTATUS_OK;
}
