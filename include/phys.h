/*
 * phys.h - 물리 주소 <-> 가상 주소 변환 (higher-half 커널 메모리 레이아웃)
 *
 * 가상 주소 공간 (x86_64, 4단계 페이징):
 *   0x0000000000000000 ~ 0x00007FFFFFFFFFFF : 사용자 공간 (커널 부팅 후에는 비어 있음)
 *   0xFFFF800000000000 ~                    : HHDM (물리 메모리 전체를 그대로 이어 붙인 직접 매핑)
 *   0xFFFFFFFF80000000 ~                    : 커널 이미지 (물리 1MiB 에 적재, 여기에 링크됨)
 *
 * 규칙:
 *  - 하드웨어(DMA, 페이지 테이블 엔트리, BAR 등)에 넘기는 주소는 "물리 주소" 이다.
 *  - 커널 코드가 포인터로 역참조하는 주소는 "가상 주소" 이다.
 *  - 두 값을 헷갈리지 않도록 물리 주소는 u64, 가상 주소는 포인터로 다룬다.
 */
#ifndef NYXIS_PHYS_H
#define NYXIS_PHYS_H

#include "types.h"

#define KERNEL_VMA_BASE  0xFFFFFFFF80000000UL   /* 커널 이미지 가상 주소 = 물리 주소 + 이 값 */
#define HHDM_BASE        0xFFFF800000000000UL   /* 물리 주소 p 는 HHDM_BASE + p 로 접근 */

/* 물리 주소를 HHDM 가상 주소로 (RAM/MMIO 모두 paging_init 이 매핑한 범위 안에서만 유효) */
static __inline__ void *phys_to_virt(u64 phys)
{
    return (void *)(usize)(phys + HHDM_BASE);
}

/*
 * 커널 이미지 또는 HHDM 안의 가상 주소를 물리 주소로.
 * 그 외 주소(사용자 공간 등)는 물리적으로 연속임을 보장할 수 없으므로 변환하지 않고 그대로 돌려준다.
 * (DMA 등 물리 주소가 꼭 필요한 곳에서는 phys_is_direct() 로 먼저 확인할 것)
 */
static __inline__ u64 virt_to_phys(const void *virt)
{
    u64 a = (u64)(usize)virt;

    if (a >= KERNEL_VMA_BASE)
        return a - KERNEL_VMA_BASE;
    if (a >= HHDM_BASE)
        return a - HHDM_BASE;
    return a;
}

/* virt 가 커널 이미지 / HHDM 안에 있어서 virt_to_phys 가 유효한지 */
static __inline__ bool virt_is_direct(const void *virt)
{
    return ((u64)(usize)virt >= HHDM_BASE) ? true : false;
}

#endif /* NYXIS_PHYS_H */
