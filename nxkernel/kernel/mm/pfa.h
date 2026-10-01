/*
 * pfa.h - 물리 프레임 할당자 (Physical Frame Allocator)
 *
 * 부팅 시 한 번 채워지는 "자유 프레임 연결 리스트" 방식이다. 각 자유 페이지는 그 페이지
 * 자신의 첫 8바이트(HHDM 가상 주소로 접근)에 "다음 자유 페이지의 물리 주소"를 저장한다.
 * 그래서 별도의 비트맵/메타데이터 배열 없이 상수 크기의 헤더(리스트 머리 포인터 하나)만으로
 * O(1) 할당/해제가 된다. (RAM 크기에 비례하는 정적 메모리가 필요 없다)
 *
 * paging_init()/paging_enable() 이 HHDM(물리 메모리 전체의 가상 별칭)을 먼저 구성해야만
 * 이 할당자가 프레임 안에 next 포인터를 쓸 수 있으므로, pfa_init() 은 반드시 그 이후에
 * 호출해야 한다. paging.c 자신의 부트스트랩(커널/HHDM 페이지 테이블)은 이 할당자보다 먼저
 * 실행되므로 별도의 정적 풀을 그대로 쓴다 (paging.c 주석 참고).
 *
 * 보안: pfa_alloc() 이 반환하는 페이지는 항상 0 으로 채워져 있다. 그러지 않으면 어떤
 * 프로세스가 반납한 메모리(예: 종료된 프로세스의 힙/스택 내용)를 다음에 그 프레임을 받는
 * 프로세스가 그대로 읽어낼 수 있다 (정보 유출).
 */
#ifndef KERNEL_MM_PFA_H
#define KERNEL_MM_PFA_H

#include "nyxis.h"
#include "types.h"

/*
 * 사용 가능한 메모리 맵(info)에서, 아래 물리 범위들을 제외한 나머지를 자유 프레임으로 등록한다.
 *   - 0 ~ 1MiB (레거시 영역: BIOS/EBDA/일부 펌웨어 예약, 신뢰하지 않고 항상 제외)
 *   - [kernel_phys_start, kernel_phys_end) : 실행 중인 커널 이미지 자신
 *   - [initrd_phys_start, initrd_phys_end) : initrd (있는 경우)
 *   - hhdm_limit 이상 (HHDM 이 매핑하지 않은 범위는 물리 접근이 애초에 불가능하다)
 *
 * 펌웨어/부트로더가 보고한 메모리 타입만으로는 부족하다: 특히 Multiboot2(E820 기반) 는
 * GRUB 이 커널/모듈을 이미 올려놓은 영역도 "사용 가능"으로 보고할 수 있으므로, 커널 이미지와
 * initrd 는 항상 명시적으로 제외해야 한다 (타입에 의존하지 않는다).
 *
 * 여러 번 호출하면 NalreadyInitialized.
 */
Nstatus pfa_init(
    const NTBLI *info,
    u64 kernel_phys_start,
    u64 kernel_phys_end,
    u64 initrd_phys_start,   /* initrd 가 없으면 initrd_phys_end 와 같은 값(범위 없음) */
    u64 initrd_phys_end,
    u64 hhdm_limit
);

/*
 * 0 으로 채워진 4KiB 프레임 하나를 할당해 물리 주소를 돌려준다.
 * 남은 프레임이 없으면 NoutOfMemory.
 */
Nstatus pfa_alloc(u64 *out_phys);

/*
 * pfa_alloc() 이 준 프레임을 반납한다. phys 는 4KiB 정렬이어야 한다.
 * (반납된 프레임의 이전 내용은 유지되지만, 다음 pfa_alloc() 이 그 프레임을 다시 내줄 때는
 *  항상 0 으로 지워서 돌려준다.)
 */
Nstatus pfa_free(u64 phys);

/* 현재 자유 프레임 개수 (진단/자체 점검용) */
u64 pfa_free_count(void);

#endif /* KERNEL_MM_PFA_H */
