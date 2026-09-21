/*
 * paging.h - x86_64 4단계 페이징 (PML4 -> PDPT -> PD -> PT), higher-half 커널 레이아웃
 *
 * [기존 코드의 문제 (전면 재작성 이유)]
 *  - 32비트 2단계 페이징(page directory + page table)을 64비트 long mode 커널에서 사용했다.
 *    롱 모드 CR3 는 반드시 PML4 를 가리켜야 한다. 포인터를 (u32) 로 잘랐고, 페이지 테이블 메모리를
 *    고정 주소 0x200000 부터 "그냥 증가" 시키며 할당해 커널 .bss 를 덮을 수 있었다.
 *  - paging_disable(): 롱 모드에서는 CR0.PG 를 끌 수 없다 (#GP). 삭제.
 *
 * [현재 설계] (phys.h 의 가상 주소 레이아웃 참고)
 *  - PML4[0..255]   : 사용자 공간. 부팅 직후에는 비어 있다. (프로세스별 CR3 는 추후)
 *  - PML4[256]      : HHDM. 물리 메모리 [0, max(4GiB, RAM 끝)) 를 2MiB 페이지로 직접 매핑 (RW, NX).
 *                     커널 이미지가 놓인 물리 범위는 HHDM 에서 읽기 전용으로 낮춘다.
 *                     (HHDM 별칭으로 커널 코드를 쓰는 W^X 우회 방지)
 *  - PML4[511]      : 커널 이미지 (VMA 0xFFFFFFFF80000000 ~). 4KiB 페이지, 섹션별 W^X:
 *                     .text = R-X, .rodata = R--(NX), .data/.bss = RW-(NX). 부트 스택 가드 페이지는 매핑하지 않는다.
 *  - 페이지 테이블 메모리는 커널 .bss 의 정적 풀에서 할당한다.
 *  - EFER.NXE, CR0.WP, (지원 시) CR4.SMEP 를 켠다.
 */
#ifndef KERNEL_PAGING_H
#define KERNEL_PAGING_H

#include "nyxis.h"

#define PAGE_SIZE       4096UL
#define PAGE_SIZE_2M    0x200000UL

/* 페이지 테이블 엔트리 플래그 */
#define PAGE_PRESENT    0x001UL
#define PAGE_RW         0x002UL
#define PAGE_USER       0x004UL
#define PAGE_PWT        0x008UL
#define PAGE_PCD        0x010UL
#define PAGE_ACCESSED   0x020UL
#define PAGE_DIRTY      0x040UL
#define PAGE_HUGE       0x080UL
#define PAGE_GLOBAL     0x100UL
#define PAGE_NX         0x8000000000000000UL

/* 사용자 공간 상한 (x86_64 canonical 하위 절반의 끝) */
#define USER_SPACE_END  0x0000800000000000UL

/*
 * 페이지 테이블을 구성한다. (CR3 는 아직 바꾸지 않는다)
 * info 의 메모리 맵/프레임버퍼 범위를 참고하여 매핑 범위를 결정한다.
 */
Nstatus paging_init(const NTBLI *info);   /* info 의 주소 필드는 아직 "물리 주소" 여야 한다 */

/* 구성한 페이지 테이블로 전환한다. (NXE/WP/SMEP 활성화 포함) paging_init 성공 후에만 호출. */
void paging_enable(void);

/*
 * 4KiB 페이지 하나를 매핑한다. phys(물리 주소)/virt 는 4KiB 정렬이어야 한다.
 * virt 는 사용자 공간(< USER_SPACE_END)이어야 한다.
 * 정책: 쓰기 가능(PAGE_RW)이면서 실행 가능(NX 없음)인 매핑은 거부한다 (W^X).
 * 이미 2MiB 항등 매핑이 있는 영역이나 이미 매핑된 페이지에는 NalreadyExists.
 */
Nstatus paging_map_page(void *phys, void *virt, u64 flags);

/* 4KiB 페이지 하나의 매핑을 해제한다. */
Nstatus paging_unmap_page(void *virt);

/*
 * [virt, virt+len) 전체가 "유저 모드에서 접근 가능"(필요하면 쓰기 가능)하게 매핑되어 있는지
 * 검사한다. 시스템 콜에서 유저 포인터를 신뢰하기 전에 반드시 사용해야 한다.
 */
bool paging_is_user_range(const void *virt, usize len, bool write);

#endif /* KERNEL_PAGING_H */
