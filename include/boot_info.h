/*
 * boot_info.h - 부트로더(NYTB) <-> 커널 사이의 부트 정보 규약
 *
 * NTBLI 구조체 자체는 types.h 에 정의되어 있다. (부트로더와 공유)
 * 이 헤더는 NTBLI 안의 UEFI 메모리 맵을 해석하기 위한 정의를 추가한다.
 */
#ifndef __BOOT_INFO_H__
#define __BOOT_INFO_H__

#include "types.h"

/* UEFI EFI_MEMORY_TYPE 값 (UEFI 규격 고정값) */
#define NTBLI_MEM_RESERVED           0U
#define NTBLI_MEM_LOADER_CODE        1U
#define NTBLI_MEM_LOADER_DATA        2U
#define NTBLI_MEM_BOOT_CODE          3U
#define NTBLI_MEM_BOOT_DATA          4U
#define NTBLI_MEM_RUNTIME_CODE       5U
#define NTBLI_MEM_RUNTIME_DATA       6U
#define NTBLI_MEM_CONVENTIONAL       7U
#define NTBLI_MEM_UNUSABLE           8U
#define NTBLI_MEM_ACPI_RECLAIM       9U
#define NTBLI_MEM_ACPI_NVS          10U
#define NTBLI_MEM_MMIO              11U
#define NTBLI_MEM_MMIO_PORT         12U
#define NTBLI_MEM_PAL_CODE          13U
#define NTBLI_MEM_PERSISTENT        14U

/*
 * EFI_MEMORY_DESCRIPTOR 의 앞부분 (UEFI 규격).
 * 실제 descriptor 간격은 NTBLI.memmap_desc_size 이며 sizeof 보다 클 수 있으므로
 * 반드시 memmap_desc_size 단위로 순회해야 한다.
 */
typedef struct {
    u32 type;
    u32 pad;
    u64 phys_start;
    u64 virt_start;
    u64 num_pages;
    u64 attribute;
} ntbli_memdesc_t;

NX_STATIC_ASSERT(ntbli_memdesc_size, sizeof(ntbli_memdesc_t) == 40);

/* 페이지 크기 (UEFI 메모리 맵의 num_pages 단위) */
#define NTBLI_PAGE_SIZE 4096UL

/*
 * idx 번째 descriptor 의 포인터를 반환한다. 범위를 벗어나면 NULL.
 * (부트로더가 준 값이므로 신뢰하되, 범위 검사는 항상 수행한다.)
 */
static __inline__ const ntbli_memdesc_t *
ntbli_memmap_at(const NTBLI *info, u64 idx)
{
    u64 count;

    if (!info || !info->memmap_base || info->memmap_desc_size < sizeof(ntbli_memdesc_t))
        return (const ntbli_memdesc_t *)0;

    count = info->memmap_size / info->memmap_desc_size;
    if (idx >= count)
        return (const ntbli_memdesc_t *)0;

    return (const ntbli_memdesc_t *)((const u8 *)info->memmap_base +
                                     idx * info->memmap_desc_size);
}

/* 메모리 맵의 descriptor 개수 (맵이 없으면 0) */
static __inline__ u64 ntbli_memmap_count(const NTBLI *info)
{
    if (!info || !info->memmap_base || info->memmap_desc_size < sizeof(ntbli_memdesc_t))
        return 0;

    return info->memmap_size / info->memmap_desc_size;
}

#endif /* __BOOT_INFO_H__ */
