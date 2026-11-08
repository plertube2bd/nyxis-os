/*
 * waitq.h - 대기 큐: 어떤 일이 일어날 때까지 프로세스를 재우는 기본 장치
 *
 * 쓰는 곳: 콘솔 입력, NxProcessWait(자식 종료), 채널 수신/송신 공간, NxPoll(여러 객체 동시 대기).
 *
 * [구조] 큐에는 프로세스가 아니라 "대기 항목(waitq_entry_t)" 이 매달린다. 한 프로세스는 동시에
 * 여러 큐에 항목을 매달 수 있다 (NxPoll). 어느 큐에서든 깨워지면 그 프로세스의 모든 항목이 모든 큐에서
 * 한꺼번에 떨어진다 — 깨어난 쪽이 조건을 다시 검사하므로 "어느 큐가 깨웠는지" 는 필요 없다.
 * 항목은 잠드는 함수의 커널 스택에 있다 (할당 없음). 잠든 동안 프로세스가 종료되면 terminate 경로가
 * waitq_cancel_locked() 로 모든 항목을 떼어 내므로, 버려지는 스택을 가리키는 항목이 큐에 남지 않는다.
 *
 * [사용 규칙] 깨움 신호를 놓치지 않으려면 "조건 검사 + 잠들기" 를 인터럽트를 끈 채로 한다:
 *
 *     u64 flags = irq_save();
 *     while (!조건) {
 *         status = waitq_sleep_locked(&q, timeout);   // 잠든 동안에는 다른 프로세스가 실행된다
 *         if (NSTATUS_IS_ERR(status)) break;          // Ntimeout 등
 *     }
 *     irq_restore(flags);
 *
 *   조건을 바꾸는 쪽(IRQ 핸들러 등)은 조건을 바꾼 뒤 waitq_wake_all()/waitq_wake_one() 을 부른다.
 *   깨어났다고 조건이 참이라는 보장은 없으므로(다른 대기자가 먼저 가져갔을 수 있다) 항상 while 로 다시 검사한다.
 *
 * [제약]
 *  - idle(pid 0)/부트 스레드는 잠들 수 없다 (Npermission). 그 스레드는 폴링해야 한다.
 *  - 잠든 동안 프로세스가 종료(NxProcessKill)될 수 있다. 이때 커널 스택은 그대로 버려지므로,
 *    잠들기 전에 해제가 필요한 자원(kmalloc 버퍼, 락 등)을 쥐고 있으면 안 된다.
 */
#ifndef KERNEL_PROCESS_WAITQ_H
#define KERNEL_PROCESS_WAITQ_H

#include "nyxis.h"

#define WAITQ_MAX_MULTI 16U     /* waitq_sleep_multi_locked 가 한 번에 기다릴 수 있는 큐 수 */

struct process;
struct waitq;

typedef struct waitq_entry {
    struct process *proc;
    struct waitq *q;
    struct waitq_entry *next_in_q;     /* 같은 큐의 다음 항목 */
    struct waitq_entry *next_of_proc;  /* 같은 프로세스의 다음 항목 (다른 큐) */
} waitq_entry_t;

typedef struct waitq {
    waitq_entry_t *head;
    waitq_entry_t *tail;
} waitq_t;

void waitq_init(waitq_t *q);

/*
 * 현재 프로세스를 q 에 매달고 깨워질 때까지 재운다. 인터럽트가 꺼진 상태에서 불러야 한다.
 *   timeout_ticks: 0 이면 무기한, 아니면 그 tick 수가 지나면 Ntimeout 으로 돌아온다.
 * 반환: NSTATUS_OK(깨워짐) / Ntimeout / Npermission(idle) / Ninterrupted(신호가 왔다, 또는 전환할 대상이 없었음)
 */
Nstatus waitq_sleep_locked(waitq_t *q, u64 timeout_ticks);

/* qs[0..n-1] 중 어느 하나에서 깨워질 때까지 잔다 (n <= WAITQ_MAX_MULTI, 중복 큐는 한 번만 매단다) */
Nstatus waitq_sleep_multi_locked(waitq_t **qs, u32 n, u64 timeout_ticks);

/* 대기자 모두/하나를 깨운다 (IRQ 핸들러에서도 안전). 깨운 프로세스 수 / 깨웠는지 반환. */
u32 waitq_wake_all(waitq_t *q);
bool waitq_wake_one(waitq_t *q);

/* p 가 기다리는 중이면 모든 큐에서 그 항목들을 뗀다 (깨울 때, 시간 초과, 프로세스 종료). IRQ 를 끈 채 호출. */
void waitq_cancel_locked(struct process *p);

#endif /* KERNEL_PROCESS_WAITQ_H */
