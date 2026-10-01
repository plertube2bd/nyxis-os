/*
 * elf.c - ELF64 사용자 프로그램 로더 구현 (설계와 제약은 elf.h 참고)
 *
 * 이 파일이 신뢰하지 않는 것: file 의 내용 전부. ELF 헤더/프로그램 헤더 테이블의 모든 값은
 * 검증 전까지 "공격자가 통제할 수 있는 입력"으로 취급한다 (부트로더의 커널 ELF 로더와 같은
 * 원칙 — bootloader/nytb/nytb3.c 의 load_elf_kernel() 참고. 다만 여기서는 목적지가 커널
 * 물리 메모리가 아니라 새 프로세스의 사용자 주소 공간이라는 점이 다르다).
 */

#include "kernel/process/elf.h"
#include "kernel/mm/pfa.h"
#include "phys.h"
#include "memory.h"

#define ELF_MAGIC       0x464C457FU
#define ET_EXEC         2U
#define EM_X86_64       62U
#define PT_LOAD         1U
#define PF_X            1U
#define PF_W            2U

#define ELFCLASS64      2U
#define ELFDATA2LSB     1U

#define ELF_MAX_PHNUM   32U

/* 사용자 ELF 파일 자체의 크기 상한 (커널이 이 파일을 통째로 올리지는 않지만, 비정상적으로
 * 큰 값(예: 헤더 수치 조작으로 인한 매우 큰 e_phoff 등)을 방지하기 위한 상식적 상한이다) */
#define NX_ELF_MAX_FILE (64UL * 1024UL * 1024UL)

/* 세그먼트 가상 주소 허용 범위: 너무 낮은(NULL 근처) 주소와, 스택/향후 다른 매핑과 섞일 수
 * 있는 지나치게 높은 주소를 모두 배제한다 (스택은 process_create_user 호출자가 훨씬 높은
 * 고정 주소에 별도로 매핑한다). */
#define NX_USER_ELF_MIN_VADDR 0x10000UL
#define NX_USER_ELF_MAX_VADDR 0x0000700000000000UL

typedef struct {
    u32 e_magic;

    u8  e_class;
    u8  e_data;
    u8  e_version;
    u8  e_osabi;
    u8  e_abiversion;
    u8  pad[7];

    u16 e_type;
    u16 e_machine;

    u32 e_version2;

    u64 e_entry;

    u64 e_phoff;
    u64 e_shoff;

    u32 e_flags;

    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;

    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} elf64_header_t;

typedef struct {
    u32 p_type;
    u32 p_flags;

    u64 p_offset;

    u64 p_vaddr;
    u64 p_paddr;

    u64 p_filesz;
    u64 p_memsz;

    u64 p_align;
} elf64_phdr_t;

NX_STATIC_ASSERT(elf64_header_size, sizeof(elf64_header_t) == 64);
NX_STATIC_ASSERT(elf64_phdr_size, sizeof(elf64_phdr_t) == 56);

static u64 align_up(u64 v, u64 a)
{
    return (v + a - 1) & ~(a - 1);
}

/* file->offset 을 offset 으로 옮긴 뒤 정확히 len 바이트를 읽는다. 짧게 읽히면 손상된 파일이다. */
static Nstatus read_exact(handle_t *file, u64 offset, void *buf, usize len)
{
    usize got = 0;
    Nstatus status;

    file->offset = offset;
    status = vfs_read(file, buf, len, &got);
    if (NSTATUS_IS_ERR(status))
        return status;
    if (got != len)
        return NfileCorrupted;

    return NSTATUS_OK;
}

/*
 * 세그먼트 하나를 4KiB 페이지 단위로 할당·매핑하고 파일 내용을 채운다.
 *
 * 실패 시 정리 정책: 이 함수는 "지금까지 이 세그먼트에서 매핑한 페이지"를 스스로 되돌리지
 * 않는다. elf_load() 가 실패하면 호출자가 항상 주소 공간(as) 전체를
 * paging_addrspace_destroy() 로 파기하도록 되어 있고, 그 함수는 사용자 영역에 남아 있는
 * "모든" 매핑(이 세그먼트의 앞쪽 페이지, 그리고 이전에 이미 로드된 다른 세그먼트들 포함)을
 * 정확히 찾아 pfa_free() 한다. 이 함수가 부분적으로만 되돌리려 하면 오히려 "매핑은
 * 해제했지만 pfa_free 는 안 된" 프레임이 생겨 물리 메모리가 새는 쪽이 더 위험하므로,
 * 정리는 항상 "주소 공간 통째로 파기" 한 곳에서만 일어나도록 통일한다.
 */
static Nstatus load_segment(handle_t *file, addr_space_t *as, const elf64_phdr_t *ph)
{
    u64 vaddr_end = align_up(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
    u64 flags = (u64)PAGE_USER;
    u64 page;

    if (ph->p_flags & PF_W)
        flags |= PAGE_RW;
    if (!(ph->p_flags & PF_X))
        flags |= PAGE_NX;

    for (page = ph->p_vaddr; page < vaddr_end; page += PAGE_SIZE) {
        u64 phys;
        u64 seg_off = page - ph->p_vaddr;
        u64 copy_len = 0;
        Nstatus status;

        status = pfa_alloc(&phys);
        if (NSTATUS_IS_ERR(status))
            return status;

        if (seg_off < ph->p_filesz) {
            u64 remain = ph->p_filesz - seg_off;

            copy_len = (remain < PAGE_SIZE) ? remain : (u64)PAGE_SIZE;
        }

        if (copy_len > 0) {
            status = read_exact(file, ph->p_offset + seg_off,
                                (void *)(usize)phys_to_virt(phys), (usize)copy_len);
            if (NSTATUS_IS_ERR(status)) {
                (void)pfa_free(phys);   /* 아직 매핑 전이라 as 의 페이지 테이블에는 없다 */
                return status;
            }
        }
        /* copy_len 이후 나머지 바이트는 pfa_alloc() 이 이미 0 으로 채워 두었다 (.bss 구간) */

        status = paging_map_page(as, (void *)(usize)phys, (void *)(usize)page, flags);
        if (NSTATUS_IS_ERR(status)) {
            (void)pfa_free(phys);       /* 여기도 마찬가지로 매핑에 실패했으니 아직 안 걸려 있다 */
            return status;
        }
    }

    return NSTATUS_OK;
}

Nstatus elf_load(handle_t *file, addr_space_t *as, u64 *out_entry, u64 *out_image_end)
{
    elf64_header_t hdr;
    elf64_phdr_t phdrs[ELF_MAX_PHNUM];
    vfs_stat_t st;
    u64 file_size;
    u64 image_end = 0;
    bool entry_ok = false;
    bool any_load = false;
    u32 i;
    Nstatus status;

    if (!file || !as || !as->pml4 || !out_entry || !out_image_end)
        return NinvalidArg;

    status = vfs_fstat(file, &st);
    if (NSTATUS_IS_ERR(status))
        return status;
    file_size = st.size;
    if (file_size == 0 || file_size > NX_ELF_MAX_FILE)
        return NinvalidFormat;

    status = read_exact(file, 0, &hdr, sizeof(hdr));
    if (NSTATUS_IS_ERR(status))
        return status;

    if (hdr.e_magic != ELF_MAGIC || hdr.e_class != ELFCLASS64 ||
        hdr.e_data != ELFDATA2LSB || hdr.e_type != ET_EXEC ||
        hdr.e_machine != EM_X86_64)
        return Nunsupported;

    if (hdr.e_phentsize != sizeof(elf64_phdr_t) || hdr.e_phnum == 0 ||
        hdr.e_phnum > ELF_MAX_PHNUM)
        return NinvalidFormat;

    if (hdr.e_phoff > file_size ||
        (u64)hdr.e_phnum * sizeof(elf64_phdr_t) > file_size - hdr.e_phoff)
        return NinvalidFormat;

    status = read_exact(file, hdr.e_phoff, phdrs, (usize)hdr.e_phnum * sizeof(elf64_phdr_t));
    if (NSTATUS_IS_ERR(status))
        return status;

    for (i = 0; i < hdr.e_phnum; i++) {
        const elf64_phdr_t *ph = &phdrs[i];
        u64 seg_end;

        if (ph->p_type != PT_LOAD)
            continue;

        if (ph->p_memsz == 0 || ph->p_filesz > ph->p_memsz)
            return NinvalidFormat;
        if (ph->p_offset > file_size || ph->p_filesz > file_size - ph->p_offset)
            return NinvalidFormat;
        if ((ph->p_vaddr & (PAGE_SIZE - 1)) != 0)
            return NinvalidFormat;      /* 페이지 정렬 요구 (elf.h 문서 참고) */
        if (ph->p_vaddr < NX_USER_ELF_MIN_VADDR || ph->p_vaddr >= NX_USER_ELF_MAX_VADDR)
            return NinvalidFormat;
        if (ph->p_memsz > NX_USER_ELF_MAX_VADDR - ph->p_vaddr)
            return NinvalidFormat;      /* 오버플로/범위 초과 */

        seg_end = align_up(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
        if (seg_end > image_end)
            image_end = seg_end;

        if ((ph->p_flags & PF_X) && hdr.e_entry >= ph->p_vaddr &&
            hdr.e_entry - ph->p_vaddr < ph->p_memsz)
            entry_ok = true;

        any_load = true;
    }

    if (!any_load || !entry_ok)
        return NinvalidFormat;

    for (i = 0; i < hdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD)
            continue;

        status = load_segment(file, as, &phdrs[i]);
        if (NSTATUS_IS_ERR(status))
            return status;              /* 호출자가 주소 공간 전체를 파기한다 */
    }

    *out_entry = hdr.e_entry;
    *out_image_end = image_end;
    return NSTATUS_OK;
}
