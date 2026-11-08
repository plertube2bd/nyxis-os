/*
 * spawn.c - 사용자 프로세스 생성 (설계는 spawn.h, 메모리 배치는 umem.h 참고)
 */

#include "kernel/process/spawn.h"
#include "kernel/process/process.h"
#include "kernel/process/elf.h"
#include "kernel/process/umem.h"
#include "kernel/mm/pfa.h"
#include "kernel/mm/kheap.h"
#include "kernel/paging/paging.h"
#include "drivers/filesystem/vfs.h"
#include "phys.h"
#include "memory.h"

/* 스택 페이지들을 할당·매핑한다. 가드 페이지(NX_USER_STACK_GUARD)는 일부러 매핑하지 않는다. */
static Nstatus map_user_stack(addr_space_t *as)
{
    u64 page;

    for (page = NX_USER_STACK_BASE; page < NX_USER_STACK_TOP; page += PAGE_SIZE) {
        u64 phys;
        Nstatus status = pfa_alloc(&phys);    /* 0 으로 채워져 있다 */

        if (NSTATUS_IS_ERR(status))
            return status;          /* 이미 매핑된 앞쪽 페이지는 호출자가 주소 공간째 파기한다 */

        status = paging_map_page(as, (void *)(usize)phys, (void *)(usize)page,
                                 (u64)(PAGE_USER | PAGE_RW | PAGE_NX));
        if (NSTATUS_IS_ERR(status)) {
            (void)pfa_free(phys);
            return status;
        }
    }

    return NSTATUS_OK;
}

/* 아직 CR3 에 실리지 않은 주소 공간 as 의 [vaddr, vaddr+len) 에 HHDM 을 통해 쓴다 (페이지 경계 처리) */
static Nstatus write_to_addrspace(addr_space_t *as, u64 vaddr, const void *src, usize len)
{
    const u8 *s = (const u8 *)src;

    while (len > 0) {
        u64 phys;
        usize chunk = (usize)(PAGE_SIZE - (vaddr & (PAGE_SIZE - 1)));
        Nstatus status = paging_translate(as, vaddr, &phys);

        if (NSTATUS_IS_ERR(status))
            return status;
        if (chunk > len)
            chunk = len;

        memcpy(phys_to_virt(phys), s, chunk);
        vaddr += chunk;
        s += chunk;
        len -= chunk;
    }

    return NSTATUS_OK;
}

/* args 의 형식을 검사한다: 상한 이내이고, NUL 로 끝나는 문자열이 정확히 argc+envc 개 */
static Nstatus validate_args(const spawn_args_t *args)
{
    usize i;
    u32 nuls = 0;

    if ((u64)args->argc + (u64)args->envc > NX_ARGV_MAX || args->size > NX_ARG_MAX)
        return Noverflow;
    if (args->size == 0)
        return (args->argc + args->envc == 0) ? NSTATUS_OK : NinvalidArg;
    if (!args->strings || args->strings[args->size - 1] != '\0')
        return NinvalidArg;

    for (i = 0; i < args->size; i++) {
        if (args->strings[i] == '\0')
            nuls++;
    }
    return (nuls == args->argc + args->envc) ? NSTATUS_OK : NinvalidArg;
}

/*
 * System V AMD64 초기 스택을 만든다 (낮은 주소 -> 높은 주소):
 *   rsp -> argc | argv[0..argc-1] | NULL | envp[0..envc-1] | NULL | AT_NULL(0) 0 | (0 채움) | 문자열들 | 스택 끝
 * rsp 는 16바이트 정렬. 문자열은 스택 맨 위에 두고 포인터는 그 문자열의 사용자 주소를 가리킨다.
 */
static Nstatus build_initial_stack(addr_space_t *as, const spawn_args_t *args, u64 *out_rsp)
{
    u32 argc = args ? args->argc : 0;
    u32 envc = args ? args->envc : 0;
    usize size = args ? args->size : 0;
    u64 words = 1UL + (argc + 1UL) + (envc + 1UL) + 2UL;
    u64 str_base = (NX_USER_STACK_TOP - size) & ~15UL;
    u64 rsp = (str_base - words * 8UL) & ~15UL;
    u64 *vec;
    u64 w = 0;
    usize off = 0;
    u32 i;
    Nstatus status;

    /* 인자가 스택의 대부분을 차지하면 프로그램이 쓸 스택이 없다 (상한상 일어나지 않지만 명시적으로 막는다) */
    if (rsp < NX_USER_STACK_BASE + NX_USER_STACK_SIZE / 2UL)
        return Noverflow;

    if (size > 0) {
        status = write_to_addrspace(as, str_base, args->strings, size);
        if (NSTATUS_IS_ERR(status))
            return status;
    }

    vec = (u64 *)kpage_alloc();           /* 최대 (NX_ARGV_MAX + 5) * 8 = 2088 바이트 < 4KiB */
    if (!vec)
        return NoutOfMemory;

    vec[w++] = argc;
    for (i = 0; i < argc + envc; i++) {
        if (i == argc)
            vec[w++] = 0;                 /* argv 끝 */
        vec[w++] = str_base + off;
        while (args->strings[off] != '\0')
            off++;
        off++;
    }
    if (envc == 0)
        vec[w++] = 0;                     /* envp 가 비어 있으면 루프 안에서 argv 끝을 못 썼다 */
    vec[w++] = 0;                         /* envp 끝 */
    vec[w++] = 0;                         /* auxv: AT_NULL */
    vec[w++] = 0;

    status = write_to_addrspace(as, rsp, vec, (usize)(w * 8UL));
    kpage_free(vec);
    if (NSTATUS_IS_ERR(status))
        return status;

    *out_rsp = rsp;
    return NSTATUS_OK;
}

Nstatus spawn_build_image(const char *path, const spawn_args_t *args, spawn_image_t *out)
{
    handle_t file;
    addr_space_t as;
    u64 entry = 0;
    u64 image_end = 0;
    u64 rsp = 0;
    Nstatus status;

    if (!path || !out)
        return NinvalidArg;
    if (args) {
        status = validate_args(args);
        if (NSTATUS_IS_ERR(status))
            return status;
    }

    /* 읽기 + 실행 권한 (보안 계층: 실행 비트가 없는 파일은 실행할 수 없다) */
    status = vfs_open(path, 1U | VFS_O_EXEC, &file);
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

    status = build_initial_stack(&as, args, &rsp);
    if (NSTATUS_IS_ERR(status)) {
        paging_addrspace_destroy(&as);
        return status;
    }

    out->as = as;
    out->entry = entry;
    out->stack = rsp;
    out->image_end = image_end;
    return NSTATUS_OK;
}

Nstatus process_spawn(const char *path, const spawn_args_t *args, process_t **out_proc)
{
    spawn_image_t img;
    Nstatus status;

    if (!path || !out_proc)
        return NinvalidArg;
    status = spawn_build_image(path, args, &img);
    if (NSTATUS_IS_ERR(status))
        return status;
    status = process_create_user(img.as, img.entry, img.stack, img.image_end, out_proc);
    if (NSTATUS_IS_ERR(status)) {
        paging_addrspace_destroy(&img.as);
        return status;
    }
    return NSTATUS_OK;
}
