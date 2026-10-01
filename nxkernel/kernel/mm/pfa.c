/*
 * pfa.c - 물리 프레임 할당자 구현 (설계는 pfa.h 참고)
 */

#include "kernel/mm/pfa.h"
#include "boot_info.h"
#include "phys.h"
#include "sync.h"
#include "memory.h"

#define PFA_PAGE_SIZE   4096UL
#define PFA_LOW_RESERVED 0x100000UL   /* 0 ~ 1MiB 은 항상 제외 */

static u64 g_free_head = 0;      /* 자유 리스트 머리 (물리 주소), 0 = 비어 있음 */
static u64 g_free_count = 0;
static bool g_initialized = false;
static spinlock_t g_pfa_lock;

static u64 align_up(u64 v, u64 a)
{
    return (v + a - 1) & ~(a - 1);
}

static u64 align_down(u64 v, u64 a)
{
    return v & ~(a - 1);
}

/* [a_start, a_end) 와 [b_start, b_end) 가 겹치는지 (둘 다 end > start 인 유효 구간이어야 함) */
static bool ranges_overlap(u64 a_start, u64 a_end, u64 b_start, u64 b_end)
{
    if (a_start >= a_end || b_start >= b_end)
        return false;
    return (a_start < b_end && b_start < a_end) ? true : false;
}

/* 프레임 하나를 자유 리스트 앞에 매단다 (락을 이미 쥔 상태에서 호출) */
static void push_free_locked(u64 phys)
{
    u64 *node = (u64 *)(usize)phys_to_virt(phys);

    *node = g_free_head;
    g_free_head = phys;
    g_free_count++;
}

Nstatus pfa_init(
    const NTBLI *info,
    u64 kernel_phys_start,
    u64 kernel_phys_end,
    u64 initrd_phys_start,
    u64 initrd_phys_end,
    u64 hhdm_limit
) {
    u64 count;
    u64 i;

    if (g_initialized)
        return NalreadyInitialized;
    if (!info)
        return NinvalidArg;

    memset(&g_pfa_lock, 0, sizeof(g_pfa_lock));
    g_free_head = 0;
    g_free_count = 0;

    count = ntbli_memmap_count(info);
    for (i = 0; i < count; i++) {
        const ntbli_memdesc_t *d = ntbli_memmap_at(info, i);
        u64 start;
        u64 end;

        if (!d)
            break;
        if (d->type != NTBLI_MEM_CONVENTIONAL)
            continue;
        if (d->num_pages > (0x1000000000000UL / NTBLI_PAGE_SIZE))
            continue;              /* 비정상적으로 큰 값: 오버플로 방지를 위해 건너뜀 */

        start = d->phys_start;
        end = start + d->num_pages * NTBLI_PAGE_SIZE;
        if (end < start)
            continue;

        /* HHDM 이 실제로 매핑한 범위 밖은 물리 접근이 불가능하므로 잘라낸다 */
        if (end > hhdm_limit)
            end = hhdm_limit;
        if (start >= end)
            continue;

        start = align_up(start, PFA_PAGE_SIZE);
        end = align_down(end, PFA_PAGE_SIZE);

        for (; start < end; start += PFA_PAGE_SIZE) {
            if (start < PFA_LOW_RESERVED)
                continue;
            if (ranges_overlap(start, start + PFA_PAGE_SIZE, kernel_phys_start, kernel_phys_end))
                continue;
            if (initrd_phys_start < initrd_phys_end &&
                ranges_overlap(start, start + PFA_PAGE_SIZE, initrd_phys_start, initrd_phys_end))
                continue;

            push_free_locked(start);
        }
    }

    g_initialized = true;
    return NSTATUS_OK;
}

Nstatus pfa_alloc(u64 *out_phys)
{
    u64 phys;
    u64 *node;

    if (!out_phys)
        return NinvalidArg;

    spin_lock(&g_pfa_lock);

    if (g_free_head == 0) {
        spin_unlock(&g_pfa_lock);
        return NoutOfMemory;
    }

    phys = g_free_head;
    node = (u64 *)(usize)phys_to_virt(phys);
    g_free_head = *node;
    g_free_count--;

    spin_unlock(&g_pfa_lock);

    /* 이전 소유자(다른 프로세스일 수 있음)의 내용이 새 소유자에게 새어나가지 않도록 지운다 */
    memset((void *)(usize)phys_to_virt(phys), 0, PFA_PAGE_SIZE);

    *out_phys = phys;
    return NSTATUS_OK;
}

Nstatus pfa_free(u64 phys)
{
    if (phys == 0 || (phys & (PFA_PAGE_SIZE - 1)) != 0)
        return NinvalidArg;

    spin_lock(&g_pfa_lock);
    push_free_locked(phys);
    spin_unlock(&g_pfa_lock);

    return NSTATUS_OK;
}

u64 pfa_free_count(void)
{
    return g_free_count;
}
