/*
 * spawn.c - 사용자 프로세스 생성 (설계는 spawn.h, 메모리 배치는 umem.h 참고)
 */

#include "kernel/process/spawn.h"
#include "kernel/process/process.h"
#include "kernel/process/elf.h"
#include "kernel/process/umem.h"
#include "kernel/mm/pfa.h"
#include "kernel/paging/paging.h"
#include "drivers/filesystem/vfs.h"
#include "phys.h"
#include "memory.h"

/*
 * 스택 페이지들을 할당·매핑하고 System V 초기 스택(argc=0, argv/envp 빈 배열, auxv AT_NULL)을 쓴다.
 * 가드 페이지(NX_USER_STACK_GUARD)는 일부러 매핑하지 않는다.
 */
static Nstatus map_user_stack(addr_space_t *as)
{
    u64 page;
    u64 top_phys = 0;

    for (page = NX_USER_STACK_BASE; page < NX_USER_STACK_TOP; page += PAGE_SIZE) {
        u64 phys;
        Nstatus status = pfa_alloc(&phys);

        if (NSTATUS_IS_ERR(status))
            return status;          /* 이미 매핑된 앞쪽 페이지는 호출자가 주소 공간째 파기한다 */

        status = paging_map_page(as, (void *)(usize)phys, (void *)(usize)page,
                                 (u64)(PAGE_USER | PAGE_RW | PAGE_NX));
        if (NSTATUS_IS_ERR(status)) {
            (void)pfa_free(phys);
            return status;
        }

        if (page + PAGE_SIZE == NX_USER_STACK_TOP)
            top_phys = phys;
    }

    /* pfa_alloc() 이 0 으로 채워 주지만, 0 이라는 사실에 암묵적으로 기대지 않고 의도를 명시한다.
     * (argc=0, argv[0]=NULL, envp[0]=NULL, auxv={AT_NULL,0}) — 나중에 argv/envp 를 지원할 때
     * 이 블록이 채워질 자리다. */
    memset((u8 *)(usize)phys_to_virt(top_phys) + (PAGE_SIZE - NX_USER_STACK_INIT), 0, NX_USER_STACK_INIT);

    return NSTATUS_OK;
}

Nstatus process_spawn(const char *path, u32 *out_pid)
{
    handle_t file;
    addr_space_t as;
    u64 entry = 0;
    u64 image_end = 0;
    Nstatus status;

    if (!path || !out_pid)
        return NinvalidArg;

    status = vfs_open(path, 1U, &file);         /* 1 = 읽기 */
    if (NSTATUS_IS_ERR(status))
        return status;

    status = paging_addrspace_create(&as);
    if (NSTATUS_IS_ERR(status)) {
        (void)vfs_close(&file);
        return status;
    }

    status = elf_load(&file, &as, &entry, &image_end);
    (void)vfs_close(&file);                     /* 성공/실패와 무관하게 파일은 더 필요 없다 */
    if (NSTATUS_IS_ERR(status)) {
        paging_addrspace_destroy(&as);
        return status;
    }

    status = map_user_stack(&as);
    if (NSTATUS_IS_ERR(status)) {
        paging_addrspace_destroy(&as);
        return status;
    }

    status = process_create_user(as, entry, NX_USER_STACK_TOP - NX_USER_STACK_INIT, image_end, out_pid);
    if (NSTATUS_IS_ERR(status)) {
        paging_addrspace_destroy(&as);
        return status;
    }

    return NSTATUS_OK;
}
