/*
 * handles.c - 프로세스별 핸들 테이블 구현 (설계는 handles.h 참고)
 */

#include "kernel/syscall/handles.h"
#include "memory.h"

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

    if (slot->type == NX_HANDLE_FILE)
        (void)vfs_close(&slot->file);

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
