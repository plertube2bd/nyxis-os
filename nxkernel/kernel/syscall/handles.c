/*
 * handles.c - 프로세스별 핸들 테이블 구현 (설계는 handles.h 참고)
 */

#include "kernel/syscall/handles.h"
#include "memory.h"
#include "kernel/process/process.h"
#include "kernel/security/auth.h"
#include "kernel/ipc/channel.h"
#include "kernel/dev/device.h"
#include "kernel/crypto/sha256.h"
#include "kernel/mm/kheap.h"
#include "kernel/ipc/pipe.h"
#include "kernel/info/info.h"
#include "console/inputs/tty.h"

/* 핸들 값 = (세대 << 32) | 슬롯 번호 */
static u64 make_handle(u32 generation, u32 index)
{
    return ((u64)generation << 32) | (u64)index;
}

void nx_handles_init(nx_handle_table_t *table)
{
    u64 h;
    nx_handle_t *slot;

    memset(table, 0, sizeof(*table));

    /* 순서가 곧 슬롯 번호: 0=stdin, 1=stdout, 2=stderr (NX_HANDLE_STDIN 등과 일치) */
    if (nx_handle_alloc(table, NX_HANDLE_CONSOLE, NX_RIGHT_READ | NX_RIGHT_STAT | NX_RIGHT_DUP, &h, &slot) == NSTATUS_OK)
        slot->console_fd = 0;
    if (nx_handle_alloc(table, NX_HANDLE_CONSOLE, NX_RIGHT_WRITE | NX_RIGHT_STAT | NX_RIGHT_DUP, &h, &slot) == NSTATUS_OK)
        slot->console_fd = 1;
    if (nx_handle_alloc(table, NX_HANDLE_CONSOLE, NX_RIGHT_WRITE | NX_RIGHT_STAT | NX_RIGHT_DUP, &h, &slot) == NSTATUS_OK)
        slot->console_fd = 2;
}

Nstatus nx_handle_alloc(nx_handle_table_t *table, u32 type, u32 rights, u64 *out_handle,
                        nx_handle_t **out_slot)
{
    u32 i;

    if (!table || !out_handle || !out_slot)
        return NinvalidArg;

    for (i = 0; i < NX_MAX_HANDLES; i++) {
        nx_handle_t *slot = &table->slots[i];

        if (slot->type != NX_HANDLE_FREE)
            continue;

        /* 세대는 유지한 채 나머지만 초기화 (재사용 시 세대는 close 에서 이미 증가시켰다) */
        {
            u32 gen = slot->generation;

            memset(slot, 0, sizeof(*slot));
            slot->generation = gen;
        }
        slot->type = type;
        slot->rights = rights & (u32)NX_RIGHT_ALL;

        *out_handle = make_handle(slot->generation, i);
        *out_slot = slot;
        return NSTATUS_OK;
    }

    return NoutOfMemory;
}

Nstatus nx_handle_lookup(nx_handle_table_t *table, u64 handle, u32 required_rights,
                         nx_handle_t **out_slot)
{
    u32 index;
    u32 generation;
    nx_handle_t *slot;

    if (!table || !out_slot)
        return NinvalidArg;

    index = (u32)(handle & 0xFFFFFFFFUL);
    generation = (u32)(handle >> 32);

    if (index >= NX_MAX_HANDLES)
        return NinvalidArg;

    slot = &table->slots[index];
    if (slot->type == NX_HANDLE_FREE || slot->generation != generation)
        return NinvalidArg;

    if ((slot->rights & required_rights) != required_rights)
        return Npermission;

    *out_slot = slot;
    return NSTATUS_OK;
}

Nstatus nx_handle_close(nx_handle_table_t *table, u64 handle)
{
    nx_handle_t *slot;
    Nstatus status = nx_handle_lookup(table, handle, 0, &slot);

    if (NSTATUS_IS_ERR(status))
        return status;

    if (slot->type == NX_HANDLE_FILE && slot->of) {
        if (--slot->of->refs == 0) {          /* 마지막 공유자: VFS 핸들을 닫는다 */
            (void)vfs_close(&slot->of->file);
            kfree(slot->of);
        }
    } else if (slot->type == NX_HANDLE_PIPE && slot->pipe)
        pipe_release(slot->pipe, slot->pipe_write);
    else if (slot->type == NX_HANDLE_CONSOLE && slot->tty)
        tty_release(slot->tty);
    else if (slot->type == NX_HANDLE_PTY && slot->pty)
        pty_master_release(slot->pty);
    else if (slot->type == NX_HANDLE_PROCESS && slot->proc)
        process_release(slot->proc);      /* 마지막 참조였고 이미 끝났으면 슬롯이 여기서 회수된다 */
    else if (slot->type == NX_HANDLE_TOKEN && slot->token) {
        crypto_wipe(slot->token, sizeof(*slot->token));
        kfree(slot->token);
    } else if (slot->type == NX_HANDLE_CHANNEL && slot->chan)
        channel_release(slot->chan, slot->chan_receiver);
    else if (slot->type == NX_HANDLE_DEVICE && slot->dev)
        device_close(slot->dev, slot->dev->owner, slot->dev_write);   /* 장치 핸들은 복제 불가: 소유자 = 이 핸들의 프로세스 */
    else if (slot->type == NX_HANDLE_INFO && slot->info_buf)
        kfree(slot->info_buf);

    {
        u32 gen = slot->generation + 1U;      /* 이 슬롯의 오래된 핸들 값을 무효화 */

        memset(slot, 0, sizeof(*slot));
        slot->generation = gen;
    }
    return NSTATUS_OK;
}

void nx_handle_discard(nx_handle_t *slot)
{
    u32 gen;

    if (!slot)
        return;

    gen = slot->generation + 1U;
    memset(slot, 0, sizeof(*slot));
    slot->generation = gen;
}

void nx_handles_close_all(nx_handle_table_t *table)
{
    u32 i;

    for (i = 0; i < NX_MAX_HANDLES; i++) {
        nx_handle_t *slot = &table->slots[i];

        if (slot->type == NX_HANDLE_FREE)
            continue;
        (void)nx_handle_close(table, make_handle(slot->generation, i));
    }
}

u32 nx_handles_count(const nx_handle_table_t *table)
{
    u32 i;
    u32 n = 0;

    for (i = 0; i < NX_MAX_HANDLES; i++) {
        if (table->slots[i].type != NX_HANDLE_FREE)
            n++;
    }
    return n;
}

u64 nx_handle_value(const nx_handle_table_t *table, u32 index)
{
    if (!table || index >= NX_MAX_HANDLES || table->slots[index].type == NX_HANDLE_FREE)
        return NX_HANDLE_INVALID;
    return make_handle(table->slots[index].generation, index);
}

nx_ofile_t *nx_ofile_new(const handle_t *f)
{
    nx_ofile_t *of = (nx_ofile_t *)kmalloc(sizeof(*of));

    if (!of)
        return nNULL;
    of->file = *f;
    of->refs = 1;
    of->append = false;
    return of;
}

Nstatus nx_handle_share(const nx_handle_t *src, nx_handle_t *dst, bool share)
{
    Nstatus status;

    switch (src->type) {
    case NX_HANDLE_CONSOLE:
        dst->console_fd = src->console_fd;
        dst->tty = src->tty;
        tty_retain(dst->tty);
        return NSTATUS_OK;
    case NX_HANDLE_PTY:
        dst->pty = src->pty;
        pty_master_retain(dst->pty);
        return NSTATUS_OK;
    case NX_HANDLE_FILE:
        if (share) {
            dst->of = src->of;
            dst->of->refs++;
            return NSTATUS_OK;
        }
        {
            /* 드라이버 상태를 공유하지 않도록 새로 연다 (오프셋은 복사, 이후 독립) */
            handle_t copy;

            status = vfs_reopen(&src->of->file, &copy);
            if (NSTATUS_IS_ERR(status))
                return status;
            dst->of = nx_ofile_new(&copy);
            if (!dst->of) {
                (void)vfs_close(&copy);
                return NoutOfMemory;
            }
            dst->of->append = src->of->append;
            return NSTATUS_OK;
        }
    case NX_HANDLE_PROCESS:
        dst->proc = src->proc;              /* 같은 프로세스를 가리키는 참조를 하나 더 */
        process_retain(dst->proc);
        return NSTATUS_OK;
    case NX_HANDLE_PIPE:
        dst->pipe = src->pipe;
        dst->pipe_write = src->pipe_write;
        pipe_retain(dst->pipe, dst->pipe_write);
        return NSTATUS_OK;
    case NX_HANDLE_CHANNEL:
        if (src->chan_receiver)
            return Nunsupported;            /* 받는 쪽은 하나 */
        dst->chan = src->chan;
        dst->chan_receiver = false;
        channel_retain(dst->chan);
        return NSTATUS_OK;
    case NX_HANDLE_INFO:
        /* 스냅샷을 복사한다 (이후 독립: 오프셋도 따로) */
        dst->info_dir = src->info_dir;
        dst->info_len = src->info_len;
        dst->info_offset = src->info_offset;
        if (src->info_buf) {
            dst->info_buf = (char *)kmalloc(INFO_VALUE_MAX);
            if (!dst->info_buf)
                return NoutOfMemory;
            memcpy(dst->info_buf, src->info_buf, (usize)src->info_len);
        }
        return NSTATUS_OK;
    default:                                /* 토큰(일회용), 장치(독점 소유) */
        return Nunsupported;
    }
}

Nstatus nx_handles_inherit(nx_handle_table_t *dst, const nx_handle_table_t *src,
                           bool keep_generation, bool skip_cloexec)
{
    u32 i;
    Nstatus result = NSTATUS_OK;

    nx_handles_close_all(dst);
    for (i = 0; i < NX_MAX_HANDLES; i++) {
        const nx_handle_t *s = &src->slots[i];
        nx_handle_t *d = &dst->slots[i];
        Nstatus status;

        memset(d, 0, sizeof(*d));
        d->generation = keep_generation ? s->generation : 0U;
        if (s->type == NX_HANDLE_FREE || (skip_cloexec && (s->hflags & NX_HFLAG_CLOEXEC)))
            continue;
        /* 프로세스 핸들은 물려주지 않는다: 부모의 자식은 자식의 자식이 아니다 */
        status = (s->type == NX_HANDLE_PROCESS) ? Nunsupported : nx_handle_share(s, d, true);
        if (status == Nunsupported) {
            /* 물려주지 않는 종류: 자식에서는 닫힌 슬롯. 부모의 값이 자식에서 우연히 다른 객체를 가리키지
             * 않도록 세대를 하나 올려 둔다 */
            memset(d, 0, sizeof(*d));
            d->generation = keep_generation ? s->generation + 1U : 0U;
            continue;
        }
        if (NSTATUS_IS_ERR(status)) {
            memset(d, 0, sizeof(*d));
            d->generation = keep_generation ? s->generation + 1U : 0U;
            result = status;
            continue;
        }
        d->type = s->type;
        d->rights = s->rights;
        d->hflags = s->hflags;
    }
    return result;
}

void nx_handles_exec(nx_handle_table_t *table)
{
    u32 i;

    for (i = 0; i < NX_MAX_HANDLES; i++) {
        nx_handle_t *slot = &table->slots[i];

        if (slot->type != NX_HANDLE_FREE && (slot->hflags & NX_HFLAG_CLOEXEC))
            (void)nx_handle_close(table, make_handle(slot->generation, i));
        slot->generation = 0;               /* 새 프로그램에서 핸들 값 = 슬롯 번호 */
    }
}
