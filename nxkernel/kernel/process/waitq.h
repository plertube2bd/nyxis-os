/*
 * waitq.h - 대기 큐: 어떤 일이 일어날 때까지 프로세스를 재우는 기본 장치
 *
 * 쓰는 곳: 콘솔 입력(키 입력을 기다림), NxProcessWait(자식 종료를 기다림). 앞으로 파이프/IPC/디스크 IRQ 도.
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

struct process;

typedef struct waitq {
    struct process *head;
    struct process *tail;
} waitq_t;

void waitq_init(waitq_t *q);

/*
 * 현재 프로세스를 q 에 넣고 깨워질 때까지 재운다. 인터럽트가 꺼진 상태에서 불러야 한다.
 *   timeout_ticks: 0 이면 무기한, 아니면 그 tick 수가 지나면 Ntimeout 으로 돌아온다.
 * 반환: NSTATUS_OK(깨워짐) / Ntimeout / Npermission(idle) / Ninterrupted(전환할 대상이 없었음)
 */
Nstatus waitq_sleep_locked(waitq_t *q, u64 timeout_ticks);

/* 대기자 모두/하나를 깨운다 (IRQ 핸들러에서도 안전). 깨운 수 / 깨웠는지 반환. */
u32 waitq_wake_all(waitq_t *q);
bool waitq_wake_one(waitq_t *q);

/* p 가 어떤 큐에서 기다리는 중이면 그 큐에서 뺀다 (프로세스 종료 시). 인터럽트가 꺼진 상태에서 호출. */
void waitq_remove_locked(struct process *p);

#endif /* KERNEL_PROCESS_WAITQ_H */
