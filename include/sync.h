/*
 * sync.h - 원자적 연산(atomic)과 스핀락
 *
 * [수정 이력 요약]
 *  - types.h 에 들어 있던 atomic/spinlock 을 분리했다.
 *    (types.h 가 lowlevel.h 의 함수를 전방 선언해서 쓰던 순환 구조 제거)
 *  - atomic 연산은 항상 lock 접두사를 사용한다.
 *    예전에는 multicore_enabled == false 이면 평범한 v->value++ 를 썼는데,
 *    컴파일러가 load/inc/store 로 쪼갤 수 있어 인터럽트 핸들러와 경쟁 가능했다.
 *  - spin_unlock 이 무조건 sti() 하던 버그 수정.
 *    이제 spin_lock 시점의 RFLAGS 를 락 안에 저장하고, unlock 때 그 상태로 복원한다.
 *    (인터럽트가 꺼진 상태에서 락을 잡았다 풀어도 인터럽트가 켜지지 않는다)
 *  - 락 획득 순서: 먼저 인터럽트를 끄고 나서 락을 잡는다.
 *    (반대 순서면 락을 든 채 인터럽트 핸들러가 같은 락을 잡으려다 교착 상태 발생)
 */
#ifndef NYXIS_SYNC_H
#define NYXIS_SYNC_H

#include "types.h"
#include "lowlevel.h"

/* 멀티코어가 활성화되었는지 여부 (kernel.c 에서 정의) */
extern bool multicore_enabled;

typedef struct {
    volatile i32 value;
} atomic_t;

static __inline__ i32 atomic_read(const atomic_t *v)
{
    return v->value;
}

static __inline__ void atomic_inc(atomic_t *v)
{
    __asm__ volatile ("lock incl %0"
                      : "+m"(v->value)
                      :
                      : "memory", "cc");
}

static __inline__ void atomic_dec(atomic_t *v)
{
    __asm__ volatile ("lock decl %0"
                      : "+m"(v->value)
                      :
                      : "memory", "cc");
}

/* v->value 가 oldval 이면 newval 로 바꾸고, 어떤 경우든 "이전 값"을 반환한다. */
static __inline__ i32 atomic_cmpxchg(atomic_t *v, i32 oldval, i32 newval)
{
    i32 prev;

    __asm__ volatile ("lock cmpxchgl %2, %1"
                      : "=a"(prev), "+m"(v->value)
                      : "r"(newval), "0"(oldval)
                      : "memory", "cc");

    return prev;
}

static __inline__ void atomic_set(atomic_t *v, i32 newval)
{
    /* xchg 는 암묵적으로 lock 이다 */
    __asm__ volatile ("xchgl %0, %1"
                      : "+r"(newval), "+m"(v->value)
                      :
                      : "memory");
}

/* 스핀락. 0 으로 초기화하면 잠기지 않은 상태. */
typedef struct {
    atomic_t locked;
    u64      saved_flags;   /* spin_lock 시점의 RFLAGS (spin_unlock 에서 복원) */
} spinlock_t;

static __inline__ void spin_lock(spinlock_t *lock)
{
    u64 flags = irq_save();

    if (multicore_enabled) {
        while (atomic_cmpxchg(&lock->locked, 0, 1) != 0) {
            while (lock->locked.value)
                cpu_pause();
        }
    }

    lock->saved_flags = flags;
}

static __inline__ void spin_unlock(spinlock_t *lock)
{
    u64 flags = lock->saved_flags;

    if (multicore_enabled)
        atomic_set(&lock->locked, 0);

    irq_restore(flags);
}

#endif /* NYXIS_SYNC_H */
