/*
 * shm.c - 공유 메모리 객체 (설계는 shm.h)
 */
#include "kernel/mm/shm.h"
#include "kernel/mm/pfa.h"
#include "kernel/mm/kheap.h"
#include "kernel/paging/paging.h"
#include "kernel/process/process.h"
#include "interrupt.h"
#include "phys.h"
#include "memory.h"

#define FRAMES_PER_INDEX (PAGE_SIZE / sizeof(u64))

static u64 g_total_pages;           /* 모든 객체의 프레임 수 (NX_SHM_TOTAL_MAX 상한) */

static u64 *frame_slot(nx_shm_t *s, u32 i)
{
    return &s->frames[i / FRAMES_PER_INDEX][i % FRAMES_PER_INDEX];
}

static void destroy(nx_shm_t *s)
{
    u32 i;

    for (i = 0; i < s->pages; i++) {
        u64 phys = *frame_slot(s, i);

        if (phys)
            (void)pfa_free(phys);
    }
    for (i = 0; i < SHM_INDEX_PAGES; i++)
        kpage_free(s->frames[i]);
    g_total_pages -= s->pages;
    kfree(s);
}

Nstatus shm_create(u64 size, nx_shm_t **out)
{
    nx_shm_t *s;
    u32 pages;
    u32 idx;
    u32 i;
    u64 flags;

    if (size == 0 || size > NX_SHM_MAX || !out)
        return NinvalidArg;
    pages = (u32)((size + PAGE_SIZE - 1) / PAGE_SIZE);

    flags = irq_save();
    if (g_total_pages + pages > NX_SHM_TOTAL_MAX / PAGE_SIZE) {
        irq_restore(flags);
        return NoutOfMemory;
    }
    g_total_pages += pages;              /* 먼저 잡아 둔다 (실패하면 destroy 가 돌려준다) */
    irq_restore(flags);

    s = (nx_shm_t *)kzalloc(sizeof(*s));
    if (!s) {
        flags = irq_save();
        g_total_pages -= pages;
        irq_restore(flags);
        return NoutOfMemory;
    }
    s->pages = pages;
    s->size = (u64)pages * PAGE_SIZE;
    s->refs = 1;
    for (idx = 0; idx * FRAMES_PER_INDEX < pages; idx++) {
        s->frames[idx] = (u64 *)kpage_alloc();       /* 0 으로 채워져 있다 */
        if (!s->frames[idx])
            goto fail;
    }
    for (i = 0; i < pages; i++) {
        u64 phys;

        if (NSTATUS_IS_ERR(pfa_alloc(&phys)))
            goto fail;
        memset(phys_to_virt(phys), 0, PAGE_SIZE);    /* 이전 내용이 새지 않게 */
        *frame_slot(s, i) = phys;
    }
    *out = s;
    return NSTATUS_OK;

fail:
    destroy(s);
    return NoutOfMemory;
}

void shm_retain(nx_shm_t *s)
{
    u64 flags = irq_save();

    s->refs++;
    irq_restore(flags);
}

void shm_release(nx_shm_t *s)
{
    u64 flags = irq_save();
    bool last = s->refs > 0 && --s->refs == 0;

    irq_restore(flags);
    if (last)
        destroy(s);
}

Nstatus shm_map(nx_shm_t *s, process_t *p, u64 va, bool write)
{
    u64 pflags = PAGE_USER | PAGE_NX | PAGE_NOFREE | (write ? PAGE_RW : 0UL);
    u32 i;

    if (!p || !p->addrspace.pml4)
        return NinvalidArg;
    for (i = 0; i < s->pages; i++) {
        Nstatus status = paging_map_page(&p->addrspace, (void *)(usize)*frame_slot(s, i),
                                         (void *)(usize)(va + (u64)i * PAGE_SIZE), pflags);

        if (NSTATUS_IS_ERR(status)) {
            u32 k;

            for (k = 0; k < i; k++)
                (void)paging_unmap_page(&p->addrspace, (void *)(usize)(va + (u64)k * PAGE_SIZE));
            return status;
        }
    }
    return NSTATUS_OK;
}

void shm_unmap(nx_shm_t *s, process_t *p, u64 va)
{
    u32 i;

    if (!p || !p->addrspace.pml4 || !va)
        return;
    for (i = 0; i < s->pages; i++)
        (void)paging_unmap_page(&p->addrspace, (void *)(usize)(va + (u64)i * PAGE_SIZE));
}
