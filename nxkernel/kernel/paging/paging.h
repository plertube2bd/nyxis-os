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
 *  - PML4[0..255]   : 사용자 공간. 프로세스마다 독립된 PML4 를 가지므로 프로세스별로 다르다
 *                     (addr_space_t 참고). 전용 주소 공간이 없는 커널 스레드는 이 범위가 항상
 *                     비어 있는 "커널 전용" PML4(paging_kernel_cr3())를 공유해서 쓴다.
 *  - PML4[256]      : HHDM. 물리 메모리 [0, max(4GiB, RAM 끝)) 를 2MiB 페이지로 직접 매핑 (RW, NX).
 *                     커널 이미지가 놓인 물리 범위는 HHDM 에서 읽기 전용으로 낮춘다.
 *                     (HHDM 별칭으로 커널 코드를 쓰는 W^X 우회 방지) 모든 주소 공간이 공유한다.
 *  - PML4[511]      : 커널 이미지 (VMA 0xFFFFFFFF80000000 ~). 4KiB 페이지, 섹션별 W^X:
 *                     .text = R-X, .rodata = R--(NX), .data/.bss = RW-(NX). 부트 스택 가드 페이지는 매핑하지 않는다.
 *                     모든 주소 공간이 공유한다 (그래야 인터럽트/시스템 콜이 어떤 프로세스의
 *                     CR3 가 실려 있든 항상 커널 코드를 실행할 수 있다).
 *  - 부팅 시점의 커널/HHDM 페이지 테이블 메모리는 커널 .bss 의 정적 풀에서 할당한다 (아직
 *    물리 프레임 할당자(pfa.h)가 준비되지 않았기 때문). paging_addrspace_create() 이후,
 *    즉 부팅이 끝난 뒤 만들어지는 모든 페이지 테이블/사용자 페이지는 pfa.h 를 통해 동적으로
 *    할당하고, paging_addrspace_destroy() 가 이를 온전히 회수한다.
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

/* paging_init() 이 실제로 HHDM 으로 매핑한 물리 주소 상한. pfa_init() 이 이 범위 밖의 프레임을
 * 자유 목록에 넣지 않도록 하는 데 쓰인다. paging_enable() 이후에만 의미가 있다. */
u64 paging_hhdm_limit(void);

/* 전용 주소 공간이 없는 프로세스(커널 스레드, idle)가 사용할 CR3 물리 주소. */
u64 paging_kernel_cr3(void);

/*
 * 프로세스별 독립 주소 공간. pml4 는 이 주소 공간의 최상위 테이블(커널 가상 주소)이다.
 * PML4 의 상위 절반(HHDM/커널 이미지, 256번·511번 항목)은 모든 주소 공간이 공유하고,
 * 하위 절반(사용자 공간, 0~255번)만 주소 공간마다 독립적이다.
 */
typedef struct {
    u64 *pml4;
} addr_space_t;

/* 새 주소 공간을 만든다 (사용자 영역은 비어 있음). pfa.h 의 물리 프레임 할당자를 사용하므로
 * paging_enable() 이후에만 호출할 수 있다. */
Nstatus paging_addrspace_create(addr_space_t *as);

/*
 * 전용 주소 공간이 없는 커널 스레드들이 공유하는 "커널 전용" 주소 공간을 addr_space_t 로
 * 감싸 돌려준다 (paging_kernel_cr3() 과 같은 PML4). 실제 프로세스 격리가 필요한 일반적인
 * 용도로는 절대 쓰면 안 된다 — 여러 커널 스레드가 항상 이 CR3 를 함께 쓰므로, 여기에 매핑한
 * 것은 시스템 전체에 보이고 paging_addrspace_destroy() 로 회수되지도 않는다(시스템 수명
 * 내내 남는다). 오직 자체 점검처럼 "진짜 프로세스 없이 매핑 API 자체를 검사"할 때만 쓴다.
 */
addr_space_t paging_kernel_addrspace(void);

/*
 * as 의 사용자 영역(PML4 0~255번)에 매핑되어 있던 모든 페이지(데이터 프레임 + 중간 페이지
 * 테이블)와 PML4 자신을 pfa_free() 로 되돌린다. 공유 영역(HHDM/커널 이미지)은 건드리지 않는다.
 * as 가 현재 CR3 로 실려 있는 동안 호출하면 안 된다 (호출자가 이미 다른 주소 공간으로 전환한
 * 뒤여야 한다).
 */
void paging_addrspace_destroy(addr_space_t *as);

/*
 * 4KiB 페이지 하나를 as 의 사용자 영역에 매핑한다. phys(물리 주소)/virt 는 4KiB 정렬이어야 한다.
 * virt 는 사용자 공간(< USER_SPACE_END)이어야 한다.
 * 정책: 쓰기 가능(PAGE_RW)이면서 실행 가능(NX 없음)인 매핑은 거부한다 (W^X).
 * 이미 매핑된 페이지에는 NalreadyExists. 중간 페이지 테이블은 pfa.h 로 동적 할당한다.
 */
Nstatus paging_map_page(addr_space_t *as, void *phys, void *virt, u64 flags);

/* as 에서 4KiB 페이지 하나의 매핑을 해제한다. (그 물리 프레임 자체를 회수하지는 않는다 —
 * 호출자가 그 프레임의 소유자이면 직접 pfa_free() 해야 한다) */
Nstatus paging_unmap_page(addr_space_t *as, void *virt);

/*
 * paging_unmap_page() 와 같지만, 그 페이지가 가리키던 물리 프레임까지 pfa_free() 로 회수한다.
 * 사용자 영역의 프레임은 항상 pfa.h 로 할당된 것이므로(파일 매핑 같은 공유 프레임이 아직 없다)
 * 힙 축소처럼 "이 프로세스가 소유한 페이지를 돌려줄 때" 쓴다.
 */
Nstatus paging_unmap_free_page(addr_space_t *as, void *virt);

/*
 * as 안에서 [virt, virt+len) 전체가 "유저 모드에서 접근 가능"(필요하면 쓰기 가능)하게
 * 매핑되어 있는지 검사한다. 시스템 콜에서 유저 포인터를 신뢰하기 전에 반드시 사용해야 하며,
 * 항상 "현재 실행 중인 프로세스"(current_process)의 주소 공간으로 호출해야 한다 — 그래야
 * 소프트웨어 검사(이 함수)와 실제 CPU 가 쓰는 CR3 가 항상 같은 주소 공간을 가리킨다.
 */
bool paging_is_user_range(const addr_space_t *as, const void *virt, usize len, bool write);

#endif /* KERNEL_PAGING_H */
