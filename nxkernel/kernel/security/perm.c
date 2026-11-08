/*
 * perm.c - 보안 계층: 권한 비트와 umask (설계는 perm.h)
 */
#include "kernel/security/perm.h"
#include "kernel/process/process.h"

u32 perm_apply_umask(u32 mode)
{
    u32 mask = current_process ? current_process->umask : NX_UMASK_DEFAULT;

    return mode & ~mask & 0777U;
}

u32 perm_set_umask(u32 mask)
{
    u32 old;

    if (!current_process)
        return NX_UMASK_DEFAULT;
    old = current_process->umask;
    current_process->umask = mask & 0777U;
    return old;
}

Nstatus perm_chmod(const char *kpath, u32 mode)
{
    if (!kpath || (mode & ~0777U) != 0)
        return NinvalidArg;             /* setuid/setgid/sticky 는 지원하지 않는다 */
    return vfs_chmod(kpath, mode);      /* 소유자/root 판정은 드라이버가 한다 */
}

void perm_fill_stat(const vfs_stat_t *vs, struct nx_stat *st)
{
    st->size = vs->size;
    st->type = (vs->type == 2U) ? NX_TYPE_DIR : NX_TYPE_FILE;
    st->mode = vs->mode & 0777U;
    st->uid = vs->uid;
    st->gid = vs->gid;
}
