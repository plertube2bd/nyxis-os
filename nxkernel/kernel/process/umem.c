/*
 * umem.c - 사용자 힙(brk) 관리 (설계는 umem.h 참고)
 */

#include "kernel/process/umem.h"
#include "kernel/mm/pfa.h"
#include "nyx_abi.h"

static u64 align_up(u64 v, u64 a)
{
    return (v + a - 1) & ~(a - 1);
}

Nstatus umem_brk(process_t *p, i64 increment, u64 *out_brk)
{
    u64 old_brk;
    u64 new_brk;
    u64 page;
    u64 first;
    u64 last;

    if (!p || !out_brk)
        return NinvalidArg;
    if (!p->addrspace.pml4)
        return Npermission;               /* 커널 스레드는 사용자 힙이 없다 */

    old_brk = p->heap_brk;

    if (increment == 0) {
        *out_brk = old_brk;
        return NSTATUS_OK;
    }

    if (increment > 0) {
        u64 inc = (u64)increment;

        if (inc > NX_HEAP_GROW_MAX_PER_CALL)
            return NinvalidArg;

        new_brk = old_brk + inc;          /* old_brk < 2^47, inc <= 256MiB 이므로 오버플로 없음 */
        if (new_brk - p->heap_start > NX_HEAP_MAX_TOTAL)
            return NoutOfMemory;
        if (new_brk >= NX_USER_STACK_GUARD)
            return NoutOfMemory;          /* 스택/가드 페이지 영역을 침범하지 않는다 */

        /* 페이지는 지금 할당하지 않는다 (요구 페이징: 처음 접근할 때 umem_demand_page 가 매핑한다).
         * 다만 지금 남은 프레임으로 채울 수도 없는 크기면 malloc 이 NULL 을 받도록 여기서 거부한다 */
        first = align_up(old_brk, PAGE_SIZE);
        last = align_up(new_brk, PAGE_SIZE);
        if ((last - first) / PAGE_SIZE > pfa_free_count())
            return NoutOfMemory;

        p->heap_brk = new_brk;
        *out_brk = new_brk;
        return NSTATUS_OK;
    }

    /* increment < 0: 줄이기 */
    if (increment == (i64)(-9223372036854775807L - 1L))
        return NinvalidArg;               /* -INT64_MIN 은 표현할 수 없다 */
    {
        u64 dec = (u64)(-increment);

        if (dec > old_brk - p->heap_start)
            return NinvalidArg;           /* heap_start 아래로는 줄일 수 없다 */
        new_brk = old_brk - dec;
    }

    first = align_up(new_brk, PAGE_SIZE); /* new_brk 가 걸친 페이지는 계속 쓰인다 */
    last = align_up(old_brk, PAGE_SIZE);

    /* 한 번도 건드리지 않아 매핑되지 않은 페이지도 있다 (paging_unmap_free_page 가 NinvalidArg 로 건너뛴다) */
    for (page = first; page < last; page += PAGE_SIZE)
        (void)paging_unmap_free_page(&p->addrspace, (void *)(usize)page);

    p->heap_brk = new_brk;
    *out_brk = new_brk;
    return NSTATUS_OK;
}

/* 요구 페이징 영역: 스택 [NX_USER_STACK_BASE, NX_USER_STACK_TOP), 힙 [heap_start, brk 를 올린 페이지 경계) */
static bool demand_region(const process_t *p, u64 page)
{
    if (page >= NX_USER_STACK_BASE && page < NX_USER_STACK_TOP)
        return true;
    return page >= p->heap_start && page < align_up(p->heap_brk, PAGE_SIZE);
}

bool umem_demand_page(process_t *p, u64 addr)
{
    u64 page = addr & ~(PAGE_SIZE - 1);
    u64 phys;

    if (!p || !p->addrspace.pml4 || addr >= USER_SPACE_END || !demand_region(p, page))
        return false;
    if (!NSTATUS_IS_ERR(paging_translate(&p->addrspace, page, &phys)))
        return false;                       /* 이미 있다: 권한 위반이지 요구 페이징이 아니다 */
    if (NSTATUS_IS_ERR(pfa_alloc(&phys)))   /* 0 으로 채워져 있다 */
        return false;
    if (NSTATUS_IS_ERR(paging_map_page(&p->addrspace, (void *)(usize)phys, (void *)(usize)page,
                                       (u64)(PAGE_USER | PAGE_RW | PAGE_NX)))) {
        (void)pfa_free(phys);
        return false;
    }
    return true;
}

void umem_prefault(process_t *p, u64 addr, usize len)
{
    u64 page;
    u64 end = addr + (u64)len;

    if (!p || len == 0 || end < addr || end > USER_SPACE_END)
        return;                             /* 범위 검사는 호출자(paging_is_user_range)가 한다 */
    for (page = addr & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
        u64 phys;

        if (demand_region(p, page) && NSTATUS_IS_ERR(paging_translate(&p->addrspace, page, &phys)))
            (void)umem_demand_page(p, page);
    }
}
