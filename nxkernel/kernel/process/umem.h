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
 * [스택 규약 (1-3 확정)]
 *  - 크기는 64KiB 고정이며 자동으로 늘어나지 않는다 (확장은 아직 지원하지 않는다).
 *  - 스택 바로 아래 4KiB 는 가드 페이지로 매핑하지 않는다. 스택은 아래로 자라므로 오버플로하면
 *    가드 페이지를 건드려 즉시 #PF 가 나고, 그 프로세스만 종료된다 (커널은 영향받지 않는다).
 *  - 스택은 항상 NX (실행 금지) + RW 다. paging_map_page 가 W^X 를 강제하므로 우연히라도
 *    RW+X 로 만들 수 없다.
 *  - 진입 시 RSP 는 16바이트 정렬이고 [rsp] 부터 System V AMD64 초기 스택 배치가 놓인다:
 *      argc(8) argv[0]=NULL(8) envp[0]=NULL(8) auxv AT_NULL(16)  -> 총 40바이트 (48 로 올림)
 *    v1 은 argc=0, argv/envp 는 빈 배열이다 (argv/envp 전달은 아직 없다).
 */
#ifndef KERNEL_PROCESS_UMEM_H
#define KERNEL_PROCESS_UMEM_H

#include "nyxis.h"
#include "kernel/process/process.h"

#define NX_USER_STACK_SIZE   (64UL * 1024UL)
#define NX_USER_STACK_TOP    0x00007FF800000000UL
#define NX_USER_STACK_BASE   (NX_USER_STACK_TOP - NX_USER_STACK_SIZE)
#define NX_USER_STACK_GUARD  (NX_USER_STACK_BASE - 4096UL)
#define NX_USER_STACK_INIT   48UL   /* 초기 argc/argv/envp/auxv 가 차지하는 바이트 (16 정렬) */

/*
 * p 의 힙(brk)을 increment 바이트만큼 키우거나(양수) 줄인다(음수). 0 이면 현재 brk 만 돌려준다.
 * 성공하면 *out_brk 에 새 brk. 실패하면 brk 는 그대로이고 이미 할당한 프레임도 모두 되돌려 둔다.
 *   - 한 번에 NX_HEAP_GROW_MAX_PER_CALL, 누적 NX_HEAP_MAX_TOTAL 을 넘으면 거부 (DoS 방지)
 *   - heap_start 아래로 줄이려 하면 NinvalidArg
 * 항상 "현재 실행 중인 프로세스"(=CR3 가 실려 있는 프로세스)에 대해서만 호출해야 한다.
 */
Nstatus umem_brk(process_t *p, i64 increment, u64 *out_brk);

#endif /* KERNEL_PROCESS_UMEM_H */
