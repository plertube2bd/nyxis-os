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

/* [from_page, to_page) 를 방금 매핑했던 순서 그대로 되돌린다 (실패 롤백용) */
static void rollback_pages(addr_space_t *as, u64 from_page, u64 to_page)
{
    u64 page;

    for (page = from_page; page < to_page; page += PAGE_SIZE)
        (void)paging_unmap_free_page(as, (void *)(usize)page);
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

        first = align_up(old_brk, PAGE_SIZE);   /* old_brk 가 걸친 페이지는 이미 매핑되어 있다 */
        last = align_up(new_brk, PAGE_SIZE);

        for (page = first; page < last; page += PAGE_SIZE) {
            u64 phys;
            Nstatus status = pfa_alloc(&phys);

            if (NSTATUS_IS_ERR(status)) {
                rollback_pages(&p->addrspace, first, page);
                return status;
            }

            status = paging_map_page(&p->addrspace, (void *)(usize)phys, (void *)(usize)page,
                                     (u64)(PAGE_USER | PAGE_RW | PAGE_NX));
            if (NSTATUS_IS_ERR(status)) {
                (void)pfa_free(phys);
                rollback_pages(&p->addrspace, first, page);
                return status;
            }
        }

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

    for (page = first; page < last; page += PAGE_SIZE)
        (void)paging_unmap_free_page(&p->addrspace, (void *)(usize)page);

    p->heap_brk = new_brk;
    *out_brk = new_brk;
    return NSTATUS_OK;
}
