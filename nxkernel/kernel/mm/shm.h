/*
 * shm.h - 공유 메모리 객체 (NxSharedMemoryCreate / NxSharedMemoryMap, 규칙은 include/nyx_abi.h)
 *
 * 객체 = 0 으로 채운 물리 프레임들 + 참조 수. 핸들(NX_HANDLE_SHM)이 참조 1 개를 가진다. 채널로 핸들을 보내면
 * (NxChannelSendHandle) 받는 쪽 핸들도 같은 객체를 가리킨다: 서로 다른 프로세스가 같은 프레임을 매핑한다.
 *
 * 매핑은 핸들에 속한다: NxSharedMemoryMap 은 그 핸들의 프로세스 주소 공간의 NX_USER_SHM_BASE 영역(핸들 슬롯마다
 * NX_SHM_SLOT_SIZE)에 매핑하고, 핸들을 닫거나(프로세스가 끝나도) exec 하면 매핑이 사라진다. 프레임은 PAGE_NOFREE 로
 * 매핑한다: 주소 공간이 사라져도 프레임은 객체의 것이고, 마지막 참조가 사라질 때 pfa 로 돌아간다.
 * fork 의 자식은 핸들은 물려받지만 매핑은 없다 (다시 매핑한다).
 */
#ifndef KERNEL_MM_SHM_H
#define KERNEL_MM_SHM_H

#include "nyxis.h"
#include "nyx_abi.h"

struct process;

#define SHM_INDEX_PAGES 8U          /* 프레임 표 (페이지 하나에 512 개): 최대 4096 프레임 = 16MiB */

typedef struct nx_shm {
    u64 size;                       /* 바이트 (페이지 단위로 올림) */
    u32 pages;
    u32 refs;
    u64 *frames[SHM_INDEX_PAGES];   /* frames[i / 512][i % 512] = 물리 주소 */
} nx_shm_t;

/* size 바이트(1 ~ NX_SHM_MAX)의 0 으로 채운 객체. 참조 1. 전체 상한(NX_SHM_TOTAL_MAX)을 넘으면 NoutOfMemory */
Nstatus shm_create(u64 size, nx_shm_t **out);
void shm_retain(nx_shm_t *s);
void shm_release(nx_shm_t *s);

/* p 의 주소 공간 va 에 객체 전체를 매핑한다 (write 면 RW). 실패하면 아무것도 남기지 않는다 */
Nstatus shm_map(nx_shm_t *s, struct process *p, u64 va, bool write);
void shm_unmap(nx_shm_t *s, struct process *p, u64 va);

#endif /* KERNEL_MM_SHM_H */
