/*
 * paging.h - x86_64 4단계 페이징 (PML4 -> PDPT -> PD -> PT)
 *
 * [기존 코드의 문제 (전면 재작성 이유)]
 *  - 32비트 2단계 페이징(page directory + page table, 엔트리 20비트 frame)을 64비트
 *    long mode 커널에서 사용하고 있었다. 롱 모드 CR3 는 반드시 PML4 를 가리켜야 한다.
 *  - 포인터를 (u32) 로 잘라서 사용 -> 4GB 이상 주소에서 잘못된 주소 사용.
 *  - 페이지 테이블용 메모리를 고정 주소 0x200000 부터 "그냥 증가" 시키며 할당했다.
 *    커널 이미지(0x100000~)가 1MB 를 넘으면 커널 자신의 .bss 를 덮어쓰게 된다.
 *  - paging_disable(): 롱 모드에서는 CR0.PG 를 끌 수 없다 (#GP). 삭제.
 *  - 접근 권한(NX/WP/유저 비트)에 대한 보안 정책이 전혀 없었다.
 *
 * [현재 설계]
 *  - 물리 메모리를 "항등 매핑(identity map)" 한다. (0 ~ max(4GiB, RAM 끝), 2MiB 페이지)
 *    x86_64 는 커널이 낮은 주소(1MiB)에 링크되어 있으므로 이 방식이 가장 단순하다.
 *  - 커널 이미지가 있는 2MiB 영역과 최하위 2MiB 는 4KiB 페이지로 쪼개서 W^X 를 적용한다:
 *      .text  = 읽기+실행(쓰기 불가), .rodata = 읽기 전용(NX), .data/.bss = 읽기/쓰기(NX)
 *    0 번 페이지(NULL 역참조 방지)와 부트 스택 가드 페이지는 매핑하지 않는다.
 *  - 페이지 테이블 메모리는 커널 .bss 안의 정적 풀에서 할당한다.
 *  - EFER.NXE, CR0.WP, (지원 시) CR4.SMEP 를 켠다.
 *
 * 한계: 사용자 공간이 아직 없다. 항등 매핑은 PML4[0] 전체를 차지하므로, 유저 프로세스를
 *       도입할 때는 상위 절반(higher-half) 커널 또는 프로세스별 CR3 설계가 필요하다.
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
Nstatus paging_init(const NTBLI *info);

/* 구성한 페이지 테이블로 전환한다. (NXE/WP/SMEP 활성화 포함) paging_init 성공 후에만 호출. */
void paging_enable(void);

/*
 * 4KiB 페이지 하나를 매핑한다. phys/virt 는 4KiB 정렬이어야 한다.
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
