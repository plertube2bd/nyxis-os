/*
 * paging.c - x86_64 4단계 페이징 구현 (설계는 paging.h 참고)
 */

#include "kernel/paging/paging.h"
#include "boot_info.h"
#include "memory.h"
#include "lowlevel.h"
#include "console/outputs/printk.h"

/* 링커 스크립트(linker64.lds)와 start.s 가 제공하는 심볼 */
extern u8 __kernel_start[];
extern u8 __text_start[];
extern u8 __text_end[];
extern u8 __rodata_start[];
extern u8 __rodata_end[];
extern u8 __data_start[];
extern u8 __kernel_end[];
extern u8 boot_stack_guard[];

#define ENTRY_ADDR_MASK   0x000FFFFFFFFFF000UL
#define ENTRIES_PER_TABLE 512U

#define IDENTITY_MIN_LIMIT  0x100000000UL   /* 최소 4GiB (PCI BAR/MMIO 대비) */
#define IDENTITY_MAX_LIMIT  0x1000000000UL  /* 최대 64GiB */
#define PML4_SLOT0_LIMIT    0x8000000000UL  /* PML4[0] 이 덮는 범위 (512GiB) */

#define MSR_EFER        0xC0000080U
#define EFER_NXE        (1UL << 11)
#define CR0_WP          (1UL << 16)
#define CR4_SMEP        (1UL << 20)

/* 페이지 테이블 전용 정적 풀 (.bss). 64GiB 항등 매핑에 PD 64개 + 여유분 */
#define PAGING_POOL_PAGES 160U

static u8 g_pool[PAGING_POOL_PAGES][PAGE_SIZE] __attribute__((aligned(4096)));
static u32 g_pool_used = 0;

static u64 *g_pml4 = nNULL;
static u64 *g_pdpt = nNULL;      /* PML4[0] 이 가리키는 PDPT (항등 매핑용) */
static bool g_nx_supported = false;
static bool g_smep_supported = false;

/* ------------------------------------------------------------------ */
/* 유틸                                                                */
/* ------------------------------------------------------------------ */

static u64 nx_bit(void)
{
    return g_nx_supported ? PAGE_NX : 0UL;
}

/* 0 으로 채워진 4KiB 테이블 하나를 풀에서 할당 */
static u64 *alloc_table(void)
{
    u64 *table;

    if (g_pool_used >= PAGING_POOL_PAGES)
        return nNULL;

    table = (u64 *)(void *)g_pool[g_pool_used++];
    memset(table, 0, PAGE_SIZE);
    return table;
}

static u64 *entry_table(u64 entry)
{
    return (u64 *)(usize)(entry & ENTRY_ADDR_MASK);
}

/* cpuid 로 NX / SMEP 지원 여부 확인 */
static void detect_cpu_features(void)
{
    u32 a, b, c, d;

    g_nx_supported = false;
    g_smep_supported = false;

    cpuid(0x80000000U, &a, &b, &c, &d);
    if (a >= 0x80000001U) {
        cpuid(0x80000001U, &a, &b, &c, &d);
        if (d & (1U << 20))
            g_nx_supported = true;
    }

    cpuid(0, &a, &b, &c, &d);
    if (a >= 7U) {
        cpuid_count(7, 0, &a, &b, &c, &d);
        if (b & (1U << 7))
            g_smep_supported = true;
    }
}

/* PDPT[idx] 가 가리키는 PD 를 얻는다 (없으면 생성) */
static u64 *get_pd(u32 pdpt_idx)
{
    u64 entry = g_pdpt[pdpt_idx];
    u64 *pd;

    if (entry & PAGE_PRESENT)
        return entry_table(entry);

    pd = alloc_table();
    if (!pd)
        return nNULL;

    g_pdpt[pdpt_idx] = (u64)(usize)pd | PAGE_PRESENT | PAGE_RW;
    return pd;
}

/* 2MiB 단위로 [start, end) 를 항등 매핑 (RW, NX) */
static Nstatus map_identity_2m(u64 start, u64 end)
{
    u64 addr;

    if (end > PML4_SLOT0_LIMIT)
        return Nunsupported;

    for (addr = start & ~(PAGE_SIZE_2M - 1); addr < end; addr += PAGE_SIZE_2M) {
        u64 *pd = get_pd((u32)((addr >> 30) & 0x1FFU));
        u32 pd_idx = (u32)((addr >> 21) & 0x1FFU);

        if (!pd)
            return NoutOfMemory;

        if (!(pd[pd_idx] & PAGE_PRESENT))
            pd[pd_idx] = addr | PAGE_PRESENT | PAGE_RW | PAGE_HUGE | nx_bit();
    }

    return NSTATUS_OK;
}

/*
 * 항등 매핑된 addr 가 속한 2MiB 영역의 PT 를 얻는다.
 * 아직 2MiB 큰 페이지이면 4KiB 페이지 512개로 쪼갠다 (같은 권한 유지).
 */
static u64 *identity_pt_for(u64 addr)
{
    u64 *pd = get_pd((u32)((addr >> 30) & 0x1FFU));
    u32 pd_idx = (u32)((addr >> 21) & 0x1FFU);
    u64 pde;
    u64 *pt;
    u32 i;

    if (!pd)
        return nNULL;

    pde = pd[pd_idx];

    if ((pde & PAGE_PRESENT) && !(pde & PAGE_HUGE))
        return entry_table(pde);

    pt = alloc_table();
    if (!pt)
        return nNULL;

    if (pde & PAGE_PRESENT) {
        u64 base = pde & ENTRY_ADDR_MASK;

        for (i = 0; i < ENTRIES_PER_TABLE; i++)
            pt[i] = (base + (u64)i * PAGE_SIZE) | PAGE_PRESENT | PAGE_RW | nx_bit();
    } else {
        /* 매핑이 없던 영역: 항등 매핑 페이지로 채움 */
        u64 base = addr & ~(PAGE_SIZE_2M - 1);

        for (i = 0; i < ENTRIES_PER_TABLE; i++)
            pt[i] = (base + (u64)i * PAGE_SIZE) | PAGE_PRESENT | PAGE_RW | nx_bit();
    }

    pd[pd_idx] = (u64)(usize)pt | PAGE_PRESENT | PAGE_RW;
    return pt;
}

/*
 * 항등 매핑된 [start, end) 의 각 4KiB 페이지 권한을 flags 로 설정한다.
 * flags == 0 이면 매핑을 제거(not present)한다.
 */
static Nstatus set_identity_flags(u64 start, u64 end, u64 flags)
{
    u64 addr;

    for (addr = start & ~(PAGE_SIZE - 1); addr < end; addr += PAGE_SIZE) {
        u64 *pt = identity_pt_for(addr);
        u32 pt_idx = (u32)((addr >> 12) & 0x1FFU);

        if (!pt)
            return NoutOfMemory;

        if (flags)
            pt[pt_idx] = addr | flags | PAGE_PRESENT;
        else
            pt[pt_idx] = 0;
    }

    return NSTATUS_OK;
}

static u64 align_up(u64 v, u64 a)
{
    return (v + a - 1) & ~(a - 1);
}

static u64 align_down(u64 v, u64 a)
{
    return v & ~(a - 1);
}

/* ------------------------------------------------------------------ */
/* 초기화                                                              */
/* ------------------------------------------------------------------ */

Nstatus paging_init(const NTBLI *info)
{
    u64 limit = IDENTITY_MIN_LIMIT;
    u64 i;
    u64 count;
    Nstatus status;

    if (!info)
        return NinvalidArg;

    detect_cpu_features();

    g_pool_used = 0;
    g_pml4 = alloc_table();
    g_pdpt = alloc_table();
    if (!g_pml4 || !g_pdpt)
        return NoutOfMemory;

    g_pml4[0] = (u64)(usize)g_pdpt | PAGE_PRESENT | PAGE_RW;

    /* RAM 으로 쓰이는 메모리 맵 항목 중 가장 높은 끝 주소까지 매핑 범위를 넓힌다 */
    count = ntbli_memmap_count(info);
    for (i = 0; i < count; i++) {
        const ntbli_memdesc_t *d = ntbli_memmap_at(info, i);
        u64 end;

        if (!d)
            break;

        /* MMIO/예약/사용불가 항목은 범위 계산에서 제외 */
        if (d->type == NTBLI_MEM_MMIO || d->type == NTBLI_MEM_MMIO_PORT ||
            d->type == NTBLI_MEM_RESERVED || d->type == NTBLI_MEM_UNUSABLE ||
            d->type == NTBLI_MEM_PAL_CODE)
            continue;

        /* 오버플로 검사 */
        if (d->num_pages > (0x1000000000000UL / NTBLI_PAGE_SIZE))
            continue;
        end = d->phys_start + d->num_pages * NTBLI_PAGE_SIZE;
        if (end < d->phys_start)
            continue;
        if (end > limit)
            limit = end;
    }

    if (limit > IDENTITY_MAX_LIMIT) {
        printk("paging: RAM above 64GiB is not mapped (top=0x%lx)\n", (unsigned long)limit);
        limit = IDENTITY_MAX_LIMIT;
    }
    limit = align_up(limit, PAGE_SIZE_2M);

    status = map_identity_2m(0, limit);
    if (NSTATUS_IS_ERR(status))
        return status;

    /* 프레임버퍼가 항등 매핑 범위 밖이면 그 영역도 매핑 */
    if (info->framebuffer_base && info->framebuffer_size) {
        u64 fb = (u64)(usize)info->framebuffer_base;
        u64 fb_end = fb + info->framebuffer_size;

        if (fb_end > fb && fb_end > limit) {
            status = map_identity_2m(align_down(fb, PAGE_SIZE_2M), align_up(fb_end, PAGE_SIZE_2M));
            if (NSTATUS_IS_ERR(status))
                return status;
        }
    }

    /* --- 최하위 2MiB: NULL 페이지를 매핑 해제 --- */
    status = set_identity_flags(0, PAGE_SIZE, 0);
    if (NSTATUS_IS_ERR(status))
        return status;

    /* --- 커널 이미지: 섹션별 W^X --- */
    {
        u64 text_s = (u64)(usize)__text_start;
        u64 text_e = align_up((u64)(usize)__text_end, PAGE_SIZE);
        u64 ro_s = (u64)(usize)__rodata_start;
        u64 ro_e = align_up((u64)(usize)__rodata_end, PAGE_SIZE);
        u64 data_s = (u64)(usize)__data_start;
        u64 data_e = align_up((u64)(usize)__kernel_end, PAGE_SIZE);
        u64 guard = (u64)(usize)boot_stack_guard;

        /* .text: 읽기 + 실행 (쓰기 불가, NX 없음).
         * flags 인자가 0 이면 "매핑 제거" 로 해석되므로 PAGE_ACCESSED 를 넣어
         * '유효 권한 없음(=R-X)' 의 비영(非0) 플래그를 만든다. */
        status = set_identity_flags(text_s, text_e, PAGE_ACCESSED);
        if (NSTATUS_IS_ERR(status))
            return status;

        /* .rodata: 읽기 전용 + NX (NX 미지원 CPU 에서도 flags != 0 이 되도록 ACCESSED 포함) */
        status = set_identity_flags(ro_s, ro_e, PAGE_ACCESSED | nx_bit());
        if (NSTATUS_IS_ERR(status))
            return status;

        /* .data/.bss: 읽기/쓰기 + NX */
        status = set_identity_flags(data_s, data_e, PAGE_RW | nx_bit());
        if (NSTATUS_IS_ERR(status))
            return status;

        /* 부트 스택 아래의 가드 페이지: 매핑 해제 (스택 오버플로 즉시 검출) */
        status = set_identity_flags(guard, guard + PAGE_SIZE, 0);
        if (NSTATUS_IS_ERR(status))
            return status;
    }

    return NSTATUS_OK;
}

void paging_enable(void)
{
    u64 cr0;

    /* NX 비트를 쓰는 테이블을 로드하기 전에 EFER.NXE 를 먼저 켜야 한다 */
    if (g_nx_supported)
        wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);

    /* 커널(ring0)도 읽기 전용 페이지에 쓸 수 없도록 WP 활성화 */
    cr0 = read_cr0();
    write_cr0(cr0 | CR0_WP);

    /* 커널이 유저 페이지의 코드를 실행하지 못하게 (지원 시) */
    if (g_smep_supported)
        write_cr4(read_cr4() | CR4_SMEP);

    write_cr3((u64)(usize)g_pml4);
}

/* ------------------------------------------------------------------ */
/* 일반 매핑 API                                                       */
/* ------------------------------------------------------------------ */

/* 정규(canonical) 주소인지 (48비트 부호 확장 확인) */
static bool is_canonical(u64 addr)
{
    u64 top = addr >> 47;
    return (top == 0UL || top == 0x1FFFFUL) ? true : false;
}

/*
 * 하위 테이블을 얻는다. create 이면 없을 때 새로 만든다.
 * user 이면 중간 엔트리에 USER 비트를 켠다 (최종 접근 권한은 leaf 가 결정).
 */
static u64 *next_level(u64 *table, u32 idx, bool create, bool user)
{
    u64 entry = table[idx];

    if (entry & PAGE_PRESENT) {
        if (entry & PAGE_HUGE)
            return nNULL;          /* 큰 페이지: 더 내려갈 수 없음 */
        if (user && !(entry & PAGE_USER))
            table[idx] = entry | PAGE_USER;
        return entry_table(entry);
    }

    if (!create) {
        return nNULL;
    } else {
        u64 *fresh = alloc_table();

        if (!fresh)
            return nNULL;
        table[idx] = (u64)(usize)fresh | PAGE_PRESENT | PAGE_RW | (user ? PAGE_USER : 0UL);
        return fresh;
    }
}

Nstatus paging_map_page(void *phys, void *virt, u64 flags)
{
    u64 p = (u64)(usize)phys;
    u64 v = (u64)(usize)virt;
    u64 *pdpt;
    u64 *pd;
    u64 *pt;
    u64 pde;
    u32 pt_idx;
    bool user;

    if (!g_pml4)
        return NnotInitialized;

    if ((p & (PAGE_SIZE - 1)) || (v & (PAGE_SIZE - 1)))
        return NinvalidArg;
    if (!virt || !is_canonical(v))
        return NinvalidArg;
    if (p >> 52)
        return NinvalidArg;

    if (!g_nx_supported)
        flags &= ~PAGE_NX;

    /* W^X: 쓰기 가능하면서 실행 가능한 매핑은 허용하지 않는다 */
    if (g_nx_supported && (flags & PAGE_RW) && !(flags & PAGE_NX))
        return Npermission;

    user = (flags & PAGE_USER) ? true : false;

    /* 상위 절반 주소는 아직 지원하지 않는다 (PML4 인덱스가 256 이상) */
    if (v >= USER_SPACE_END)
        return Nunsupported;

    /* PML4[idx] -> PDPT: 항등 매핑이 PML4[0] 을 쓰므로, 그 외 인덱스는 새로 만든다 */
    pdpt = next_level(g_pml4, (u32)((v >> 39) & 0x1FFU), true, user);
    if (!pdpt)
        return NoutOfMemory;

    /* PDPT[idx] 가 1GiB 큰 페이지이면 next_level 이 NULL 을 반환 */
    pd = next_level(pdpt, (u32)((v >> 30) & 0x1FFU), true, user);
    if (!pd)
        return NalreadyExists;

    pde = pd[(v >> 21) & 0x1FFU];
    if ((pde & PAGE_PRESENT) && (pde & PAGE_HUGE))
        return NalreadyExists;      /* 2MiB 항등 매핑 영역은 쪼개지 않는다 */

    pt = next_level(pd, (u32)((v >> 21) & 0x1FFU), true, user);
    if (!pt)
        return NoutOfMemory;

    pt_idx = (u32)((v >> 12) & 0x1FFU);
    if (pt[pt_idx] & PAGE_PRESENT)
        return NalreadyExists;

    pt[pt_idx] = p | (flags & (PAGE_RW | PAGE_USER | PAGE_PWT | PAGE_PCD | PAGE_GLOBAL | PAGE_NX))
                   | PAGE_PRESENT;

    invlpg(virt);
    return NSTATUS_OK;
}

Nstatus paging_unmap_page(void *virt)
{
    u64 v = (u64)(usize)virt;
    u64 *pdpt;
    u64 *pd;
    u64 *pt;

    if (!g_pml4)
        return NnotInitialized;
    if (!virt || (v & (PAGE_SIZE - 1)) || !is_canonical(v) || v >= USER_SPACE_END)
        return NinvalidArg;

    pdpt = next_level(g_pml4, (u32)((v >> 39) & 0x1FFU), false, false);
    if (!pdpt)
        return NinvalidArg;
    pd = next_level(pdpt, (u32)((v >> 30) & 0x1FFU), false, false);
    if (!pd)
        return NinvalidArg;
    pt = next_level(pd, (u32)((v >> 21) & 0x1FFU), false, false);
    if (!pt)
        return NinvalidArg;

    if (!(pt[(v >> 12) & 0x1FFU] & PAGE_PRESENT))
        return NinvalidArg;

    pt[(v >> 12) & 0x1FFU] = 0;
    invlpg(virt);
    return NSTATUS_OK;
}

/* 한 페이지가 유저 접근 가능한지: 모든 단계의 U(및 write 시 RW) 비트를 확인 */
static bool user_page_ok(u64 v, bool write)
{
    u64 e;
    u64 *t = g_pml4;
    u32 idx[4];
    u32 level;

    idx[0] = (u32)((v >> 39) & 0x1FFU);
    idx[1] = (u32)((v >> 30) & 0x1FFU);
    idx[2] = (u32)((v >> 21) & 0x1FFU);
    idx[3] = (u32)((v >> 12) & 0x1FFU);

    for (level = 0; level < 4; level++) {
        e = t[idx[level]];

        if (!(e & PAGE_PRESENT))
            return false;
        if (!(e & PAGE_USER))
            return false;
        if (write && !(e & PAGE_RW))
            return false;

        /* 큰 페이지(PDPT/PD 레벨)이면 여기서 종료 */
        if (level >= 1 && level <= 2 && (e & PAGE_HUGE))
            return true;

        if (level < 3)
            t = entry_table(e);
    }

    return true;
}

bool paging_is_user_range(const void *virt, usize len, bool write)
{
    u64 start = (u64)(usize)virt;
    u64 end;
    u64 page;

    if (!g_pml4)
        return false;
    if (len == 0)
        return true;

    end = start + (u64)len;
    if (end < start)                 /* 오버플로 */
        return false;
    if (end > USER_SPACE_END)        /* 커널 영역 침범 */
        return false;

    for (page = start & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
        if (!user_page_ok(page, write))
            return false;
    }

    return true;
}
