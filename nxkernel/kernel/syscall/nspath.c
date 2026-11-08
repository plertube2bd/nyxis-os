/*
 * nspath.c - 사용자 경로 해석 (규칙은 nspath.h 참고)
 */

#include "kernel/syscall/nspath.h"
#include "kernel/process/process.h"
#include "string.h"
#include "memory.h"

#define NSP_PREFIX_MAX 8U
#define NSP_COMPONENT_MAX 255U
#define NSP_MAX_DEPTH 64U

static const char g_protected[] = "fs:/roi/users";

/* out 에 s 를 이어 붙인다 */
static Nstatus append(char *out, usize max, usize *len, const char *s, usize n)
{
    if (*len + n + 1U > max)
        return NpathTooLong;
    memcpy(out + *len, s, n);
    *len += n;
    out[*len] = '\0';
    return NSTATUS_OK;
}

Nstatus nsp_translate(const char *user_path, char *out, usize out_max, u32 *out_kind)
{
    char prefix[NSP_PREFIX_MAX + 1U];
    const char *base;                   /* 별칭이 가리키는 VFS 뿌리 */
    const char *rest;
    const char *starts[NSP_MAX_DEPTH];  /* 정규화된 구성요소 (원본 문자열 안의 위치) */
    usize lens[NSP_MAX_DEPTH];
    u32 depth = 0;
    u32 kind = NSP_KIND_VFS;
    bool root_only = false;
    bool roi = false;                   /* roi:/ - 일반 사용자는 자기 uid 공간만 */
    process_t *self = current_process;
    usize len = 0;
    usize i;
    Nstatus status;

    if (!user_path || !out || out_max == 0 || !out_kind || !self)
        return NinvalidArg;

    /* 접두어: 소문자/숫자 1~8 자 + ':' */
    for (i = 0; user_path[i] != ':'; i++) {
        char c = user_path[i];

        if (i >= NSP_PREFIX_MAX || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            return (c == '\0' || c == '/') ? NnotFound : NinvalidArg;
        prefix[i] = c;
    }
    if (i == 0)
        return NnotFound;
    prefix[i] = '\0';
    rest = user_path + i + 1;

    if (strcmp(prefix, "fs") == 0) {
        base = "fs:";
        root_only = true;
    } else if (strcmp(prefix, "app") == 0) {
        base = "fs:/usr";
    } else if (strcmp(prefix, "roi") == 0) {
        base = "fs:/roi";
        roi = true;
    } else if (strcmp(prefix, "info") == 0) {
        base = "";
        kind = NSP_KIND_INFO;
    } else if (strcmp(prefix, "face") == 0) {
        base = nNULL;                    /* 아래에서 사용자 이름으로 만든다 */
    } else if (strcmp(prefix, "tmp") == 0) {
        base = "tmp:";
    } else if (strcmp(prefix, "disk") == 0) {
        base = "disk:";
    } else if (strcmp(prefix, "dev") == 0) {
        base = "";
        kind = NSP_KIND_DEV;            /* uid 0 만 (표준 스트림 이름은 예외: 아래 nsp_dev_is_public) */
    } else if (strcmp(prefix, "svc") == 0) {
        base = "";
        kind = NSP_KIND_SVC;
    } else {
        return NnotFound;
    }

    if (root_only && self->uid != 0)
        return Npermission;

    /* 구성요소 정규화 */
    while (*rest) {
        const char *s;
        usize n = 0;

        while (*rest == '/')
            rest++;
        if (*rest == '\0')
            break;
        s = rest;
        while (rest[n] != '\0' && rest[n] != '/') {
            if (rest[n] == ':')
                return NinvalidArg;        /* VFS 접두어 해석과 섞이지 않도록 */
            n++;
        }
        rest += n;

        if (n > NSP_COMPONENT_MAX)
            return NpathTooLong;
        if (n == 1 && s[0] == '.')
            continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            if (depth == 0)
                return Npermission;        /* 별칭의 뿌리 위로 나갈 수 없다 */
            depth--;
            continue;
        }
        if (depth >= NSP_MAX_DEPTH)
            return NpathTooLong;
        starts[depth] = s;
        lens[depth] = n;
        depth++;
    }

    /*
     * roi:/ 의 사용자 경계: uid 0 이 아니면 첫 구성요소가 자기 uid(10진수)인 경로만 허용한다
     * (roi:/ 자체, roi:/default, roi:/hostname, 남의 uid 공간은 Npermission). 정규화가 끝난 구성요소로
     * 검사하므로 roi:/1000/../0 같은 경로도 막힌다. 공간 안의 세부 권한은 nyfs 모드/소유자가 정한다.
     */
    if (roi && self->uid != 0) {
        char want[11];
        u32 v = self->uid;
        usize wn = 0;
        usize k;

        do {
            want[wn++] = (char)('0' + (v % 10U));
            v /= 10U;
        } while (v);
        if (depth == 0 || lens[0] != wn)
            return Npermission;
        for (k = 0; k < wn; k++) {
            if (starts[0][k] != want[wn - 1U - k])
                return Npermission;
        }
    }

    out[0] = '\0';
    if (kind != NSP_KIND_VFS) {
        /* 장치/서비스 이름: 구성요소를 '/' 로 잇는다 (앞에 '/' 없음) */
        for (i = 0; i < depth; i++) {
            if (i > 0 && NSTATUS_IS_ERR(status = append(out, out_max, &len, "/", 1)))
                return status;
            if (NSTATUS_IS_ERR(status = append(out, out_max, &len, starts[i], lens[i])))
                return status;
        }
        if (depth == 0 && kind != NSP_KIND_INFO && kind != NSP_KIND_DEV)
            return NnotFound;
        /* dev:/ 자체(이름 "")는 누구나 나열할 수 있다: 목록에는 그 사용자가 열 수 있는 것만 나온다 */
        if (kind == NSP_KIND_DEV && self->uid != 0 && depth != 0 && !nsp_dev_is_public(out))
            return Npermission;
        *out_kind = kind;
        return NSTATUS_OK;
    }

    if (base) {
        if (NSTATUS_IS_ERR(status = append(out, out_max, &len, base, strlen(base))))
            return status;
    } else {
        usize nl = strnlen(self->user_name, NX_USER_NAME_MAX);

        if (nl == 0 || nl >= NX_USER_NAME_MAX || strchr(self->user_name, '/') || strchr(self->user_name, ':'))
            return NinvalidState;
        if (NSTATUS_IS_ERR(status = append(out, out_max, &len, "fs:/home/", 9)) ||
            NSTATUS_IS_ERR(status = append(out, out_max, &len, self->user_name, nl)))
            return status;
    }

    for (i = 0; i < depth; i++) {
        if (NSTATUS_IS_ERR(status = append(out, out_max, &len, "/", 1)) ||
            NSTATUS_IS_ERR(status = append(out, out_max, &len, starts[i], lens[i])))
            return status;
    }
    if (out[len - 1] == ':' && NSTATUS_IS_ERR(status = append(out, out_max, &len, "/", 1)))
        return status;                   /* "fs:" -> "fs:/" */

    *out_kind = NSP_KIND_VFS;
    return NSTATUS_OK;
}

bool nsp_dev_is_public(const char *name)
{
    return name && (strcmp(name, "stdin") == 0 || strcmp(name, "stdout") == 0 ||
                    strcmp(name, "stderr") == 0 || strcmp(name, "tty") == 0);
}

Nstatus nsp_check_protected(const char *vfs_path, u32 op)
{
    usize plen = strlen(g_protected);
    usize vlen;

    if (!vfs_path)
        return NinvalidArg;
    vlen = strlen(vfs_path);

    /* 보호 경로 자체, 또는 그 아래 */
    if (strncmp(vfs_path, g_protected, plen) == 0 && (vfs_path[plen] == '\0' || vfs_path[plen] == '/'))
        return Npermission;

    /* 조상을 지우거나 옮기면 보호 경로도 함께 움직이므로 막는다 (fs:/ 와 fs:/roi) */
    if (op == NSP_OP_MODIFY && vlen < plen && strncmp(g_protected, vfs_path, vlen) == 0 &&
        (g_protected[vlen] == '/' || vfs_path[vlen - 1] == '/'))
        return Npermission;

    return NSTATUS_OK;
}
