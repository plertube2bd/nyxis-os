/*
 * umem.h - 사용자 프로세스의 메모리 배치 규약과 힙(brk) 관리
 *
 * [사용자 가상 주소 배치]  (프로세스마다 독립된 주소 공간이므로 다른 프로세스와 겹칠 걱정은 없다)
 *
 *   0x0000000000010000 ~  : ELF PT_LOAD 세그먼트 (elf.c 가 NX_USER_ELF_MIN_VADDR 이상만 허용)
 *   ELF 이미지 끝(heap_start) ~ : 힙 (NxVirtualAlloc 이 위로 키운다)
 *   ...                       (수십 TiB 의 빈 공간)
 *   NX_USER_STACK_TOP - NX_USER_STACK_SIZE - PAGE_SIZE : 가드 페이지 (매핑하지 않음 -> 접근 즉시 #PF)
 *   NX_USER_STACK_TOP - NX_USER_STACK_SIZE ~ NX_USER_STACK_TOP : 스택 (RW, NX, USER)
 *
 * [요구 페이징]
 *  스택 영역과 힙(brk) 영역은 주소만 예약한다. 물리 프레임은 그 페이지를 처음 건드릴 때 할당한다:
 *    - 사용자 모드의 #PF(페이지 없음)  -> umem_demand_page (interrupt.c)
 *    - 커널이 사용자 메모리를 복사할 때 -> umem_prefault (uaccess.c, 범위 검사 전에)
 *  그래서 64KiB 스택이나 malloc 이 늘린 brk 라도 실제로 쓴 페이지만큼만 RAM 을 차지한다.
 *  fork 는 있는 페이지만 복사하고, 없는 페이지는 자식에서도 처음 건드릴 때 0 으로 생긴다 (부모도 0 이었다).
 *
 * [스택 규약 (1-3 확정)]
 *  - 크기는 64KiB 고정이며 자동으로 늘어나지 않는다 (예약 크기. 프레임은 요구 페이징).
 *  - 스택 바로 아래 4KiB 는 가드 페이지로 매핑하지 않는다. 스택은 아래로 자라므로 오버플로하면
 *    가드 페이지를 건드려 즉시 #PF 가 나고, 그 프로세스만 종료된다 (커널은 영향받지 않는다).
 *  - 스택은 항상 NX (실행 금지) + RW 다. paging_map_page 가 W^X 를 강제하므로 우연히라도
 *    RW+X 로 만들 수 없다.
 *  - 진입 시 RSP 는 16바이트 정렬이고 [rsp] 부터 System V AMD64 초기 스택 배치가 놓인다:
 *      argc | argv[0..argc-1] | NULL | envp[0..envc-1] | NULL | auxv AT_NULL(16) | 0 채움 | 문자열들
 *    문자열은 스택 맨 위(NX_USER_STACK_TOP 바로 아래)에 놓인다. 인자가 없으면 40바이트(48 로 올림).
 *    (구현: spawn.c 의 build_initial_stack, 상한: nyx_abi.h 의 NX_ARG_MAX/NX_ARGV_MAX)
 */
#ifndef KERNEL_PROCESS_UMEM_H
#define KERNEL_PROCESS_UMEM_H

#include "nyxis.h"
#include "kernel/process/process.h"

#define NX_USER_STACK_SIZE   (64UL * 1024UL)
#define NX_USER_STACK_TOP    0x00007FF800000000UL
#define NX_USER_STACK_BASE   (NX_USER_STACK_TOP - NX_USER_STACK_SIZE)
#define NX_USER_STACK_GUARD  (NX_USER_STACK_BASE - 4096UL)

/* 장치 MMIO 매핑 자리 (NxDeviceMapMMIO). ELF 상한(0x7000_0000_0000) 위, 스택 아래. 힙은 누적 512MiB 상한이라
 * 여기까지 올라오지 않는다. */
#define NX_USER_MMIO_BASE    0x0000780000000000UL
#define NX_USER_MMIO_SIZE    (256UL * 1024UL * 1024UL)

/*
 * p 의 힙(brk)을 increment 바이트만큼 키우거나(양수) 줄인다(음수). 0 이면 현재 brk 만 돌려준다.
 * 성공하면 *out_brk 에 새 brk. 실패하면 brk 는 그대로이고 이미 할당한 프레임도 모두 되돌려 둔다.
 *   - 한 번에 NX_HEAP_GROW_MAX_PER_CALL, 누적 NX_HEAP_MAX_TOTAL 을 넘으면 거부 (DoS 방지)
 *   - heap_start 아래로 줄이려 하면 NinvalidArg
 * 항상 "현재 실행 중인 프로세스"(=CR3 가 실려 있는 프로세스)에 대해서만 호출해야 한다.
 */
Nstatus umem_brk(process_t *p, i64 increment, u64 *out_brk);

/*
 * addr 이 p 의 요구 페이징 영역(스택, 힙)에 있고 아직 매핑되지 않았으면 0 으로 채운 프레임을 매핑하고 true.
 * 영역 밖이거나 이미 매핑되어 있거나(권한 위반) 메모리가 없으면 false (호출자가 원래대로 폴트를 처리한다).
 */
bool umem_demand_page(process_t *p, u64 addr);

/* [addr, addr+len) 중 요구 페이징 영역에서 아직 없는 페이지를 미리 매핑한다 (커널의 사용자 메모리 복사 전에).
 * 실패해도 조용히 넘어간다: 이어지는 범위 검사가 거부한다. */
void umem_prefault(process_t *p, u64 addr, usize len);

#endif /* KERNEL_PROCESS_UMEM_H */
