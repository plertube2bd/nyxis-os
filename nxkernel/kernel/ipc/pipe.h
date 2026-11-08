/*
 * pipe.h - 파이프 (IPC 계층): 이름 없는 단방향 바이트 스트림
 *
 * NxPipeCreate 가 읽기 끝/쓰기 끝 핸들 두 개를 만든다. 두 끝은 dup/fork/상속으로 여러 핸들이 될 수 있고,
 * 파이프는 각 끝의 참조 수(readers/writers)를 센다. 둘 다 0 이 되면 해제된다.
 *
 * 의미 (POSIX 파이프와 같다):
 *  - 읽기: 데이터가 있으면 있는 만큼(최대 len) 돌려준다. 비어 있고 쓰는 쪽이 남아 있으면 잠든다.
 *          비어 있고 쓰는 쪽이 모두 닫혔으면 0 (EOF).
 *  - 쓰기: 읽는 쪽이 모두 닫혔으면 Ndisconnected (POSIX 의 EPIPE. 신호는 없다).
 *          len 을 모두 쓸 때까지 공간이 날 때마다 나눠 쓰며 잠든다. NX_PIPE_ATOMIC 이하의 쓰기는
 *          한 번에 들어간다 (다른 쓰기와 섞이지 않는다).
 *  - NxPoll: 읽기 끝은 데이터가 있거나 쓰는 쪽이 없으면 IN (쓰는 쪽이 없으면 HUP 도),
 *            쓰기 끝은 공간이 있으면 OUT, 읽는 쪽이 없으면 HUP.
 * 버퍼는 NX_PIPE_CAPACITY 바이트 링 버퍼 (커널 페이지 하나).
 *
 * 잠든 동안 프로세스가 강제 종료되어도 안전하다: 잠들기 전에 쥔 자원이 없고, 파이프는 그 프로세스의
 * 핸들이 참조를 쥐고 있어서 종료 정리(핸들 닫기) 전에는 해제되지 않는다.
 */
#ifndef KERNEL_IPC_PIPE_H
#define KERNEL_IPC_PIPE_H

#include "nyxis.h"
#include "kernel/process/waitq.h"

typedef struct nx_pipe {
    u8  *buf;           /* NX_PIPE_CAPACITY 바이트 (kpage) */
    u32 head;           /* 다음에 읽을 위치 */
    u32 count;          /* 들어 있는 바이트 수 */
    u32 readers;        /* 읽기 끝 참조 수 */
    u32 writers;        /* 쓰기 끝 참조 수 */
    waitq_t rq;         /* 데이터를 기다리는 읽기 */
    waitq_t wq;         /* 공간을 기다리는 쓰기 */
} nx_pipe_t;

/* readers = writers = 1 인 새 파이프 */
Nstatus pipe_create(nx_pipe_t **out);

/* 한쪽 끝의 참조를 하나 늘리거나 줄인다. 양쪽이 모두 0 이 되면 해제한다 */
void pipe_retain(nx_pipe_t *p, bool write_end);
void pipe_release(nx_pipe_t *p, bool write_end);

/* 사용자 버퍼와 직접 주고받는다. 반환: 바이트 수 (읽기의 0 = EOF) 또는 음수 Nstatus */
i64 pipe_read_user(nx_pipe_t *p, u64 ubuf, u64 len);
i64 pipe_write_user(nx_pipe_t *p, u64 ubuf, u64 len);

/* NxPoll 지원 */
u32 pipe_poll_bits(const nx_pipe_t *p, bool write_end);
waitq_t *pipe_waitq(nx_pipe_t *p, bool write_end);

#endif /* KERNEL_IPC_PIPE_H */
