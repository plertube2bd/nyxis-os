/*
 * kheap.h - 커널 힙 (kmalloc/kfree) 과 페이지 단위 할당 (kpage_alloc/kpage_free)
 *
 * [구조] pfa.h(물리 프레임) + HHDM 위의 크기별 slab 할당자.
 *  - 크기 클래스: 16, 32, 64, 128, 256, 512, 1024, 2048 바이트. 요청 크기는 가장 가까운 큰 클래스로 올린다.
 *  - 클래스마다 "빈 칸이 있는 페이지" 목록을 가진다. 각 페이지(4KiB 프레임)의 처음 64바이트는 헤더
 *    (매직, 클래스, 사용 중인 칸 수, 칸별 사용 비트맵)이고 나머지가 같은 크기의 칸들이다.
 *  - 페이지의 칸이 전부 비면 그 페이지는 즉시 pfa_free() 로 돌려준다 (힙이 메모리를 붙잡아 두지 않는다
 *    — 자체 점검의 "프레임 누수 없음" 검사가 힙 사용 후에도 그대로 성립한다).
 *  - 2048 바이트를 넘는 요청은 kmalloc 이 거부한다(NULL). 4KiB 가 필요하면 kpage_alloc() 을 쓴다.
 *    여러 페이지에 걸친 연속 가상 메모리(vmalloc)는 아직 없다.
 *
 * [안전성]
 *  - kfree() 는 포인터가 정말 힙의 칸 시작인지(헤더 매직, 칸 경계, 사용 비트)를 검사하고, 아니면 패닉한다.
 *    이중 해제와 힙이 아닌 포인터의 해제는 조용한 메모리 손상 대신 즉시 드러난다.
 *  - 해제한 칸은 0xDE 로 채운다 (해제 후 사용(use-after-free)이 눈에 띄게).
 *  - 인터럽트를 끈 채로 동작하므로 IRQ 핸들러에서 불러도 된다 (단일 코어 전제).
 */
#ifndef KERNEL_MM_KHEAP_H
#define KERNEL_MM_KHEAP_H

#include "nyxis.h"

#define KHEAP_MAX_ALLOC 2048UL

/* size 바이트(1 ~ KHEAP_MAX_ALLOC)를 할당한다. 내용은 정해지지 않는다. 실패하면 NULL. 16바이트 정렬. */
void *kmalloc(usize size);

/* kmalloc 과 같지만 0 으로 채워 준다 */
void *kzalloc(usize size);

/* kmalloc/kzalloc 이 돌려준 포인터를 해제한다. NULL 은 무시. 잘못된 포인터/이중 해제는 패닉. */
void kfree(void *ptr);

/* 0 으로 채워진 4KiB 페이지 하나 (HHDM 주소). 실패하면 NULL. */
void *kpage_alloc(void);

/* kpage_alloc 이 돌려준 페이지를 해제한다. NULL 은 무시. */
void kpage_free(void *page);

/* 통계 (자체 점검용): 현재 할당된 칸 수, 힙이 쥐고 있는 slab 페이지 수 */
u64 kheap_live_objects(void);
u64 kheap_slab_pages(void);

#endif /* KERNEL_MM_KHEAP_H */
