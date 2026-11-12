/*
 * syscall_ext.c - 실행 계층(fork/exec), IPC 계층(파이프), 보안 계층(stat/chmod/umask), 핸들 확장(dup2/CLOEXEC)
 *                 의 시스템 콜 진입점. 실제 일은 각 계층이 한다 (kernel/exec, kernel/ipc/pipe, kernel/security/perm).
 *                 ABI 설명은 include/nyx_abi.h.
 */
#include "kernel/syscall/syscall_internal.h"
#include "kernel/syscall/uaccess.h"
#include "kernel/syscall/nspath.h"
#include "kernel/exec/exec.h"
#include "kernel/ipc/pipe.h"
#include "console/inputs/tty.h"
#include "kernel/security/perm.h"
#include "kernel/mm/kheap.h"
#include "kernel/process/process.h"
#include "kernel/process/spawn.h"
#include "drivers/filesystem/vfs.h"
#include "nyx_abi.h"
#include "memory.h"

/* ---------------- 핸들 ---------------- */

/* NxDuplicateHandleEx(handle, rights_mask, flags, target_slot) -> 새 핸들 */
i64 sys_duplicate_handle_ex(const u64 *a)
{
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *src;
    nx_handle_t *dst;
    u64 mask = a[1];
    u64 flags = a[2];
    bool share = (flags & NX_DUP_SHARE) != 0;
    u32 hflags = (flags & NX_DUP_CLOEXEC) ? (u32)NX_HFLAG_CLOEXEC : 0U;
    u64 handle;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;
    if ((mask & ~NX_RIGHT_ALL) != 0
        || (flags & ~(NX_DUP_SHARE | NX_DUP_TO_SLOT | NX_DUP_CLOEXEC | NX_DUP_MIN_SLOT)) != 0
        || ((flags & NX_DUP_TO_SLOT) && (flags & NX_DUP_MIN_SLOT)))
        return (i64)NinvalidArg;
    status = nx_handle_lookup(table, a[0], (u32)NX_RIGHT_DUP, &src);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    if (flags & NX_DUP_TO_SLOT) {
        u32 target = (u32)a[3];
        nx_handle_t tmp;
        u32 gen;

        if (a[3] >= NX_MAX_HANDLES)
            return (i64)NinvalidArg;
        if (target == (u32)(a[0] & 0xFFFFFFFFUL))
            return (i64)a[0];               /* dup2(fd, fd): 아무것도 하지 않는다 */

        /* 새 내용을 먼저 만든다 (실패하면 대상 슬롯을 건드리지 않는다) */
        memset(&tmp, 0, sizeof(tmp));
        status = nx_handle_share(src, &tmp, share);
        if (NSTATUS_IS_ERR(status))
            return (i64)status;
        tmp.type = src->type;
        tmp.rights = src->rights & (u32)mask;
        tmp.hflags = hflags;

        handle = nx_handle_value(table, target);
        if (handle != NX_HANDLE_INVALID)
            (void)nx_handle_close(table, handle);   /* 세대가 하나 올라간다 */
        dst = &table->slots[target];
        gen = dst->generation;
        *dst = tmp;
        dst->generation = gen;
        return (i64)nx_handle_value(table, target);
    }

    if (flags & NX_DUP_MIN_SLOT) {
        u32 t;

        if (a[3] >= NX_MAX_HANDLES)
            return (i64)NinvalidArg;
        for (t = (u32)a[3]; t < NX_MAX_HANDLES && nx_handle_value(table, t) != NX_HANDLE_INVALID; t++)
            ;
        if (t == NX_MAX_HANDLES)
            return (i64)NoutOfMemory;
        dst = &table->slots[t];
        {
            u32 gen = dst->generation;

            memset(dst, 0, sizeof(*dst));
            dst->generation = gen;
        }
        status = nx_handle_share(src, dst, share);
        if (NSTATUS_IS_ERR(status)) {
            nx_handle_discard(dst);
            return (i64)status;
        }
        dst->type = src->type;
        dst->rights = src->rights & (u32)mask;
        dst->hflags = hflags;
        return (i64)nx_handle_value(table, t);
    }

    status = nx_handle_alloc(table, src->type, src->rights & (u32)mask, &handle, &dst);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    status = nx_handle_share(src, dst, share);
    if (NSTATUS_IS_ERR(status)) {
        nx_handle_discard(dst);
        return (i64)status;
    }
    dst->hflags = hflags;
    return (i64)handle;
}

/* NxHandleFlags(handle, set, clear) -> 플래그 */
i64 sys_handle_flags(const u64 *a)
{
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *slot;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;
    if (((a[1] | a[2]) & ~NX_HFLAG_CLOEXEC) != 0)
        return (i64)NinvalidArg;
    status = nx_handle_lookup(table, a[0], 0, &slot);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    slot->hflags = (slot->hflags | (u32)a[1]) & ~(u32)a[2];
    return (i64)slot->hflags;
}

/* ---------------- 실행 계층 ---------------- */

/* NxProcessFork() -> 부모: 자식 프로세스 핸들, 자식: 0 */
i64 sys_process_fork(const u64 *a)
{
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *slot;
    process_t *child = nNULL;
    u64 handle;
    Nstatus status;

    (void)a;
    if (!table || !current_process || !current_process->uframe)
        return (i64)NnotInitialized;

    /* 핸들 자리를 먼저 확보한다 (자식은 이 슬롯을 프로세스 핸들로 보고 물려받지 않는다) */
    status = nx_handle_alloc(table, NX_HANDLE_PROCESS,
                             (u32)(NX_RIGHT_WAIT | NX_RIGHT_KILL | NX_RIGHT_STAT | NX_RIGHT_DUP),
                             &handle, &slot);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    status = exec_fork(current_process->uframe, &child);
    if (NSTATUS_IS_ERR(status)) {
        nx_handle_discard(slot);
        return (i64)status;
    }
    slot->proc = child;                     /* exec_fork 가 준 참조를 핸들이 소유한다 */
    return (i64)handle;
}

/* NxProcessExec(path, argv, envp) -> 성공하면 돌아오지 않는다 (새 프로그램이 rax = 0 으로 시작) */
i64 sys_process_exec(const u64 *a)
{
    char path[NSP_PATH_MAX];
    spawn_args_t args;
    char *blob;
    usize used = 0;
    u32 total = 0;
    u32 argc;
    Nstatus status;

    if (!current_process || !current_process->uframe)
        return (i64)NnotInitialized;
    status = user_vfs_path(a[0], path, sizeof(path), NSP_OP_OPEN);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;

    blob = (char *)kpage_alloc();           /* 인자는 옛 사용자 메모리에 있다: 버리기 전에 커널로 옮긴다 */
    if (!blob)
        return (i64)NoutOfMemory;
    status = collect_user_strings(a[1], blob, &used, &total);
    argc = total;
    if (!NSTATUS_IS_ERR(status))
        status = collect_user_strings(a[2], blob, &used, &total);
    if (NSTATUS_IS_ERR(status)) {
        kpage_free(blob);
        return (i64)status;
    }
    args.argc = argc;
    args.envc = total - argc;
    args.size = used;
    args.strings = blob;

    status = exec_replace(current_process->uframe, path, &args);
    kpage_free(blob);
    return (i64)status;                     /* 성공이면 0: 새 프로그램의 rax */
}

/* ---------------- IPC 계층 ---------------- */

/* NxPipeCreate(u64 out[2], flags) -> 0 */
i64 sys_pipe_create(const u64 *a)
{
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *rs;
    nx_handle_t *ws;
    u64 h[2];
    nx_pipe_t *p;
    u32 hflags = (a[1] & NX_DUP_CLOEXEC) ? (u32)NX_HFLAG_CLOEXEC : 0U;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;
    if (a[1] & ~NX_DUP_CLOEXEC)
        return (i64)NinvalidArg;

    status = nx_handle_alloc(table, NX_HANDLE_PIPE, (u32)(NX_RIGHT_READ | NX_RIGHT_STAT | NX_RIGHT_DUP), &h[0], &rs);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    status = nx_handle_alloc(table, NX_HANDLE_PIPE, (u32)(NX_RIGHT_WRITE | NX_RIGHT_STAT | NX_RIGHT_DUP), &h[1], &ws);
    if (NSTATUS_IS_ERR(status)) {
        nx_handle_discard(rs);
        return (i64)status;
    }
    status = pipe_create(&p);
    if (NSTATUS_IS_ERR(status)) {
        nx_handle_discard(ws);
        nx_handle_discard(rs);
        return (i64)status;
    }
    rs->pipe = p;
    rs->pipe_write = false;
    rs->hflags = hflags;
    ws->pipe = p;
    ws->pipe_write = true;
    ws->hflags = hflags;

    status = copy_to_user(a[0], h, sizeof(h));
    if (NSTATUS_IS_ERR(status)) {
        (void)nx_handle_close(table, h[0]);     /* 두 끝을 닫으면 파이프도 해제된다 */
        (void)nx_handle_close(table, h[1]);
        return (i64)status;
    }
    return 0;
}

/* NxPtyCreate(u64 out[2], flags) -> 0. out[0] = 주 끝, out[1] = 종속 끝 (콘솔 핸들) */
i64 sys_pty_create(const u64 *a)
{
    nx_handle_table_t *table = cur_handles();
    nx_handle_t *ms;
    nx_handle_t *ss;
    u64 h[2];
    struct pty *p;
    u32 rights = (u32)(NX_RIGHT_READ | NX_RIGHT_WRITE | NX_RIGHT_STAT | NX_RIGHT_DUP);
    u32 hflags = (a[1] & NX_DUP_CLOEXEC) ? (u32)NX_HFLAG_CLOEXEC : 0U;
    Nstatus status;

    if (!table)
        return (i64)NnotInitialized;
    if (a[1] & ~NX_DUP_CLOEXEC)
        return (i64)NinvalidArg;

    status = nx_handle_alloc(table, NX_HANDLE_PTY, rights, &h[0], &ms);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    status = nx_handle_alloc(table, NX_HANDLE_CONSOLE, rights, &h[1], &ss);
    if (NSTATUS_IS_ERR(status)) {
        nx_handle_discard(ms);
        return (i64)status;
    }
    status = pty_create(&p);                /* 주 끝 참조 1, 종속 끝 참조 1 */
    if (NSTATUS_IS_ERR(status)) {
        nx_handle_discard(ss);
        nx_handle_discard(ms);
        return (i64)status;
    }
    ms->pty = p;
    ms->hflags = hflags;
    ss->tty = pty_tty(p);
    ss->console_fd = 0;                     /* 입력을 받는 쪽 (poll, 에코 설정) */
    ss->hflags = hflags;

    status = copy_to_user(a[0], h, sizeof(h));
    if (NSTATUS_IS_ERR(status)) {
        (void)nx_handle_close(table, h[1]);
        (void)nx_handle_close(table, h[0]);     /* 두 끝을 닫으면 pty 도 해제된다 */
        return (i64)status;
    }
    return 0;
}

/* ---------------- 보안 계층 ---------------- */

/* NxStatPath(path, struct nx_stat *out) */
i64 sys_stat_path(const u64 *a)
{
    char kpath[NSP_PATH_MAX];
    vfs_stat_t vs;
    struct nx_stat st;
    Nstatus status = user_vfs_path(a[0], kpath, sizeof(kpath), NSP_OP_OPEN);

    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    status = vfs_stat_path(kpath, &vs);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    memset(&st, 0, sizeof(st));
    perm_fill_stat(&vs, &st);
    return (i64)copy_to_user(a[1], &st, sizeof(st));
}

/* NxSetMode(path, mode) */
i64 sys_set_mode(const u64 *a)
{
    char kpath[NSP_PATH_MAX];
    Nstatus status;

    if (a[1] & ~0777UL)
        return (i64)NinvalidArg;
    status = user_vfs_path(a[0], kpath, sizeof(kpath), NSP_OP_MODIFY);
    if (NSTATUS_IS_ERR(status))
        return (i64)status;
    return (i64)perm_chmod(kpath, (u32)a[1]);
}

/* NxSetUmask(mask) -> 이전 마스크 */
i64 sys_set_umask(const u64 *a)
{
    if (a[0] & ~0777UL)
        return (i64)NinvalidArg;
    return (i64)perm_set_umask((u32)a[0]);
}
