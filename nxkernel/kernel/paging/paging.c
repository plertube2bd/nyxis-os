/*
 * paging.c - x86_64 4단계 페이징 구현 (설계는 paging.h, 주소 레이아웃은 phys.h 참고)
 */

#include "kernel/paging/paging.h"
#include "boot_info.h"
#include "phys.h"
#include "memory.h"
#include "lowlevel.h"
#include "console/outputs/printk.h"

/* 링커 스크립트(linker64.lds)와 boot.s 가 제공하는 심볼 (모두 가상 주소) */
extern u8 __boot_start[];
extern u8 __text_start[];
extern u8 __text_end[];
extern u8 __rodata_start[];
extern u8 __rodata_end[];
extern u8 __data_start[];
extern u8 __kernel_end[];
extern u8 boot_stack_guard[];

#define ENTRY_ADDR_MASK   0x000FFFFFFFFFF000UL
#define ENTRIES_PER_TABLE 512U

#define HHDM_MIN_LIMIT    0x100000000UL     /* 최소 4GiB (PCI BAR/MMIO 대비) */
#define HHDM_MAX_LIMIT    0x1000000000UL    /* 최대 64GiB */
#define PDPT_SPAN_LIMIT   0x8000000000UL    /* PDPT 하나가 덮는 범위 (512GiB) */

#define KERNEL_PML4_INDEX  511U
#define KERNEL_PDPT_INDEX  510U
#define HHDM_PML4_INDEX    256U

#define MSR_EFER        0xC0000080U
#define EFER_NXE        (1UL << 11)
#define CR0_WP          (1UL << 16)
#define CR4_SMEP        (1UL << 20)

/* 페이지 테이블 전용 정적 풀 (.bss): HHDM PD 64개 + 커널 PT + 사용자 매핑용 여유분 */
#define PAGING_POOL_PAGES 320U

static u8 g_pool[PAGING_POOL_PAGES][PAGE_SIZE] __attribute__((aligned(4096)));
static u32 g_pool_used = 0;

static u64 *g_pml4 = nNULL;
static u64 *g_pdpt_hhdm = nNULL;
static u64 *g_pd_kernel = nNULL;
static bool g_nx_supported = false;
static bool g_smep_supported = false;

/* ------------------------------------------------------------------ */
/* 유틸                                                                */
/* ------------------------------------------------------------------ */

static u64 nx_bit(void)
{
    return g_nx_supported ? PAGE_NX : 0UL;
}

/* 0 으로 채워진 4KiB 테이블 하나를 풀에서 할당 (가상 주소 반환) */
static u64 *alloc_table(void)
{
    u64 *table;

    if (g_pool_used >= PAGING_POOL_PAGES)
        return nNULL;

    table = (u64 *)(void *)g_pool[g_pool_used++];
    memset(table, 0, PAGE_SIZE);
    return table;
}

/*
 * 엔트리가 가리키는 하위 테이블의 가상 주소.
 * 모든 페이지 테이블은 커널 이미지(.bss 풀) 안에 있으므로 "물리 + KERNEL_VMA_BASE" 로 항상 접근할 수 있다.
 * (HHDM 이 아직 없는 paging_init 도중에도, CR3 전환 이후에도 동일하게 동작)
 */
static u64 *table_of(u64 entry)
{
    return (u64 *)(usize)((entry & ENTRY_ADDR_MASK) + KERNEL_VMA_BASE);
}

static u64 table_phys(const u64 *table)
{
    return virt_to_phys(table);
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

static u64 align_up(u64 v, u64 a)
{
    return (v + a - 1) & ~(a - 1);
}

static u64 align_down(u64 v, u64 a)
{
    return v & ~(a - 1);
}

/* ------------------------------------------------------------------ */
/* HHDM                                                                */
/* ------------------------------------------------------------------ */

/* HHDM PDPT[idx] 가 가리키는 PD 를 얻는다 (없으면 생성) */
static u64 *hhdm_pd(u32 pdpt_idx)
{
    u64 entry = g_pdpt_hhdm[pdpt_idx];
    u64 *pd;

    if (entry & PAGE_PRESENT)
        return table_of(entry);

    pd = alloc_table();
    if (!pd)
        return nNULL;

    g_pdpt_hhdm[pdpt_idx] = table_phys(pd) | PAGE_PRESENT | PAGE_RW;
    return pd;
}

/* 물리 [start, end) 를 2MiB 페이지로 HHDM 에 매핑 (RW, NX) */
static Nstatus map_hhdm_2m(u64 start, u64 end)
{
    u64 addr;

    if (end > PDPT_SPAN_LIMIT)
        return Nunsupported;

    for (addr = align_down(start, PAGE_SIZE_2M); addr < end; addr += PAGE_SIZE_2M) {
        u64 *pd = hhdm_pd((u32)((addr >> 30) & 0x1FFU));
        u32 pd_idx = (u32)((addr >> 21) & 0x1FFU);

        if (!pd)
            return NoutOfMemory;

        if (!(pd[pd_idx] & PAGE_PRESENT))
            pd[pd_idx] = addr | PAGE_PRESENT | PAGE_RW | PAGE_HUGE | nx_bit();
    }

    return NSTATUS_OK;
}

/* HHDM 의 물리 주소 addr 가 속한 2MiB 영역을 4KiB 페이지로 쪼개고 그 PT 를 반환 */
static u64 *hhdm_pt_for(u64 addr)
{
    u64 *pd = hhdm_pd((u32)((addr >> 30) & 0x1FFU));
    u32 pd_idx = (u32)((addr >> 21) & 0x1FFU);
    u64 pde;
    u64 *pt;
    u64 base;
    u32 i;

    if (!pd)
        return nNULL;

    pde = pd[pd_idx];
    if ((pde & PAGE_PRESENT) && !(pde & PAGE_HUGE))
        return table_of(pde);

    pt = alloc_table();
    if (!pt)
        return nNULL;

    base = align_down(addr, PAGE_SIZE_2M);
    for (i = 0; i < ENTRIES_PER_TABLE; i++)
        pt[i] = (base + (u64)i * PAGE_SIZE) | PAGE_PRESENT | PAGE_RW | nx_bit();

    pd[pd_idx] = table_phys(pt) | PAGE_PRESENT | PAGE_RW;
    return pt;
}

/* HHDM 에서 물리 [start, end) 를 읽기 전용(+NX)으로 낮춘다 */
static Nstatus hhdm_make_readonly(u64 start, u64 end)
{
    u64 addr;

    for (addr = align_down(start, PAGE_SIZE); addr < end; addr += PAGE_SIZE) {
        u64 *pt = hhdm_pt_for(addr);

        if (!pt)
            return NoutOfMemory;

        pt[(addr >> 12) & 0x1FFU] = addr | PAGE_PRESENT | nx_bit();
    }

    return NSTATUS_OK;
}

/* ------------------------------------------------------------------ */
/* 커널 이미지 (PML4[511])                                              */
/* ------------------------------------------------------------------ */

/* 가상 주소 virt(커널 이미지 안)의 페이지를 flags 로 매핑. 물리 주소 = virt - KERNEL_VMA_BASE */
static Nstatus map_kernel_page(u64 virt, u64 flags)
{
    u32 pd_idx = (u32)(((virt - KERNEL_VMA_BASE) >> 21) & 0x1FFU);
    u32 pt_idx = (u32)((virt >> 12) & 0x1FFU);
    u64 *pt;

    if (!(g_pd_kernel[pd_idx] & PAGE_PRESENT)) {
        pt = alloc_table();
        if (!pt)
            return NoutOfMemory;
        g_pd_kernel[pd_idx] = table_phys(pt) | PAGE_PRESENT | PAGE_RW;
    } else {
        pt = table_of(g_pd_kernel[pd_idx]);
    }

    pt[pt_idx] = (virt - KERNEL_VMA_BASE) | flags | PAGE_PRESENT;
    return NSTATUS_OK;
}

static Nstatus map_kernel_image(void)
{
    u64 text_s = (u64)(usize)__text_start;
    u64 text_e = (u64)(usize)__text_end;
    u64 ro_s = (u64)(usize)__rodata_start;
    u64 ro_e = (u64)(usize)__rodata_end;
    u64 data_s = (u64)(usize)__data_start;
    u64 data_e = align_up((u64)(usize)__kernel_end, PAGE_SIZE);
    u64 guard = (u64)(usize)boot_stack_guard;
    u64 v;
    Nstatus status;

    for (v = text_s; v < text_e; v += PAGE_SIZE) {         /* .text: R-X */
        status = map_kernel_page(v, 0);
        if (NSTATUS_IS_ERR(status))
            return status;
    }
    for (v = ro_s; v < ro_e; v += PAGE_SIZE) {             /* .rodata: R-- */
        status = map_kernel_page(v, nx_bit());
        if (NSTATUS_IS_ERR(status))
            return status;
    }
    for (v = data_s; v < data_e; v += PAGE_SIZE) {         /* .data/.bss: RW- */
        if (v == guard)
            continue;                                      /* 스택 가드 페이지는 매핑하지 않는다 */
        status = map_kernel_page(v, PAGE_RW | nx_bit());
        if (NSTATUS_IS_ERR(status))
            return status;
    }

    return NSTATUS_OK;
}

/* ------------------------------------------------------------------ */
/* 초기화                                                              */
/* ------------------------------------------------------------------ */

Nstatus paging_init(const NTBLI *info)
{
    u64 limit = HHDM_MIN_LIMIT;
    u64 i;
    u64 count;
    u64 *pdpt_kernel;
    Nstatus status;

    if (!info)
        return NinvalidArg;

    detect_cpu_features();

    g_pool_used = 0;
    g_pml4 = alloc_table();
    g_pdpt_hhdm = alloc_table();
    pdpt_kernel = alloc_table();
    g_pd_kernel = alloc_table();
    if (!g_pml4 || !g_pdpt_hhdm || !pdpt_kernel || !g_pd_kernel)
        return NoutOfMemory;

    g_pml4[HHDM_PML4_INDEX] = table_phys(g_pdpt_hhdm) | PAGE_PRESENT | PAGE_RW;
    g_pml4[KERNEL_PML4_INDEX] = table_phys(pdpt_kernel) | PAGE_PRESENT | PAGE_RW;
    pdpt_kernel[KERNEL_PDPT_INDEX] = table_phys(g_pd_kernel) | PAGE_PRESENT | PAGE_RW;

    /* RAM 으로 쓰이는 메모리 맵 항목 중 가장 높은 끝 주소까지 HHDM 범위를 넓힌다 */
    count = ntbli_memmap_count(info);
    for (i = 0; i < count; i++) {
        const ntbli_memdesc_t *d = ntbli_memmap_at(info, i);
        u64 end;

        if (!d)
            break;

        if (d->type == NTBLI_MEM_MMIO || d->type == NTBLI_MEM_MMIO_PORT ||
            d->type == NTBLI_MEM_RESERVED || d->type == NTBLI_MEM_UNUSABLE ||
            d->type == NTBLI_MEM_PAL_CODE)
            continue;

        if (d->num_pages > (0x1000000000000UL / NTBLI_PAGE_SIZE))
            continue;
        end = d->phys_start + d->num_pages * NTBLI_PAGE_SIZE;
        if (end < d->phys_start)
            continue;
        if (end > limit)
            limit = end;
    }

    if (limit > HHDM_MAX_LIMIT) {
        printk("paging: RAM above 64GiB is not mapped (top=0x%lx)\n", (unsigned long)limit);
        limit = HHDM_MAX_LIMIT;
    }
    limit = align_up(limit, PAGE_SIZE_2M);

    status = map_hhdm_2m(0, limit);
    if (NSTATUS_IS_ERR(status))
        return status;

    /* 프레임버퍼(물리 주소)가 HHDM 범위 밖이면 그 영역도 매핑 */
    if (info->framebuffer_base && info->framebuffer_size) {
        u64 fb = (u64)(usize)info->framebuffer_base;
        u64 fb_end = fb + info->framebuffer_size;

        if (fb_end > fb && fb_end > limit) {
            status = map_hhdm_2m(align_down(fb, PAGE_SIZE_2M), align_up(fb_end, PAGE_SIZE_2M));
            if (NSTATUS_IS_ERR(status))
                return status;
        }
    }

    /* 커널 이미지(부트 섹션 포함)가 놓인 물리 범위는 HHDM 에서 읽기 전용 */
    status = hhdm_make_readonly((u64)(usize)__boot_start,
                                align_up((u64)(usize)__kernel_end - KERNEL_VMA_BASE, PAGE_SIZE));
    if (NSTATUS_IS_ERR(status))
        return status;

    return map_kernel_image();
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

    write_cr3(table_phys(g_pml4));
}

/* ------------------------------------------------------------------ */
/* 일반 매핑 API (사용자 공간)                                          */
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
        return table_of(entry);
    }

    if (!create) {
        return nNULL;
    } else {
        u64 *fresh = alloc_table();

        if (!fresh)
            return nNULL;
        table[idx] = table_phys(fresh) | PAGE_PRESENT | PAGE_RW | (user ? PAGE_USER : 0UL);
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
    u32 pt_idx;
    bool user;

    if (!g_pml4)
        return NnotInitialized;

    if ((p & (PAGE_SIZE - 1)) || (v & (PAGE_SIZE - 1)))
        return NinvalidArg;
    if (!virt || !is_canonical(v) || v >= USER_SPACE_END)
        return NinvalidArg;
    if (p >> 52)
        return NinvalidArg;

    if (!g_nx_supported)
        flags &= ~PAGE_NX;

    /* W^X: 쓰기 가능하면서 실행 가능한 매핑은 허용하지 않는다 */
    if (g_nx_supported && (flags & PAGE_RW) && !(flags & PAGE_NX))
        return Npermission;

    user = (flags & PAGE_USER) ? true : false;

    pdpt = next_level(g_pml4, (u32)((v >> 39) & 0x1FFU), true, user);
    if (!pdpt)
        return NoutOfMemory;
    pd = next_level(pdpt, (u32)((v >> 30) & 0x1FFU), true, user);
    if (!pd)
        return NoutOfMemory;
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
            t = table_of(e);
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
