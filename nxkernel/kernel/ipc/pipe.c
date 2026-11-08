/*
 * pipe.c - 파이프 (설계는 pipe.h 참고)
 *
 * 링 버퍼와 사용자 메모리 사이는 copy_to_user/copy_from_user 로 직접 옮긴다 (중간 버퍼 없음).
 * 조건 검사와 잠들기는 인터럽트를 끈 채로 한다 (waitq.h 의 사용 규칙).
 */
#include "kernel/ipc/pipe.h"
#include "kernel/process/process.h"
#include "kernel/syscall/uaccess.h"
#include "kernel/mm/kheap.h"
#include "nyx_abi.h"
#include "lowlevel.h"
#include "memory.h"

Nstatus pipe_create(nx_pipe_t **out)
{
    nx_pipe_t *p;

    if (!out)
        return NinvalidArg;
    p = (nx_pipe_t *)kmalloc(sizeof(*p));
    if (!p)
        return NoutOfMemory;
    memset(p, 0, sizeof(*p));
    p->buf = (u8 *)kpage_alloc();
    if (!p->buf) {
        kfree(p);
        return NoutOfMemory;
    }
    p->readers = 1;
    p->writers = 1;
    waitq_init(&p->rq);
    waitq_init(&p->wq);
    *out = p;
    return NSTATUS_OK;
}

void pipe_retain(nx_pipe_t *p, bool write_end)
{
    u64 flags = irq_save();

    if (write_end)
        p->writers++;
    else
        p->readers++;
    irq_restore(flags);
}

void pipe_release(nx_pipe_t *p, bool write_end)
{
    u64 flags = irq_save();

    if (write_end) {
        if (p->writers > 0 && --p->writers == 0)
            (void)waitq_wake_all(&p->rq);   /* 읽는 쪽: 이제 EOF */
    } else {
        if (p->readers > 0 && --p->readers == 0)
            (void)waitq_wake_all(&p->wq);   /* 쓰는 쪽: 이제 끊김 */
    }
    if (p->readers == 0 && p->writers == 0) {
        kpage_free(p->buf);
        memset(p, 0, sizeof(*p));
        kfree(p);
    }
    irq_restore(flags);
}

i64 pipe_read_user(nx_pipe_t *p, u64 ubuf, u64 len)
{
    u64 flags;
    u32 n;
    u32 first;
    Nstatus status;

    if (len == 0)
        return 0;
    flags = irq_save();
    while (p->count == 0 && p->writers > 0) {
        status = waitq_sleep_locked(&p->rq, 0);
        if (NSTATUS_IS_ERR(status)) {
            irq_restore(flags);
            return (i64)status;
        }
    }
    if (p->count == 0) {                    /* 쓰는 쪽이 모두 닫혔다 */
        irq_restore(flags);
        return 0;
    }
    n = (len < (u64)p->count) ? (u32)len : p->count;
    first = NX_PIPE_CAPACITY - p->head;
    if (first > n)
        first = n;
    status = copy_to_user(ubuf, p->buf + p->head, first);
    if (!NSTATUS_IS_ERR(status) && n > first)
        status = copy_to_user(ubuf + first, p->buf, n - first);
    if (NSTATUS_IS_ERR(status)) {           /* 사용자 버퍼가 잘못됨: 아무것도 소비하지 않는다 */
        irq_restore(flags);
        return (i64)status;
    }
    p->head = (p->head + n) % NX_PIPE_CAPACITY;
    p->count -= n;
    (void)waitq_wake_all(&p->wq);
    irq_restore(flags);
    return (i64)n;
}

i64 pipe_write_user(nx_pipe_t *p, u64 ubuf, u64 len)
{
    u64 total = 0;
    u64 flags;
    Nstatus status;

    if (len == 0)
        return 0;
    flags = irq_save();
    while (total < len) {
        u32 space;
        u32 need;
        u32 n;
        u32 tail;
        u32 first;

        if (p->readers == 0) {
            irq_restore(flags);
            return total ? (i64)total : (i64)Ndisconnected;
        }
        space = NX_PIPE_CAPACITY - p->count;
        /* NX_PIPE_ATOMIC 이하의 쓰기는 통째로 들어갈 자리가 날 때까지 기다린다 (섞이지 않게) */
        need = (total == 0 && len <= NX_PIPE_ATOMIC) ? (u32)len : 1U;
        if (space < need) {
            status = waitq_sleep_locked(&p->wq, 0);
            if (NSTATUS_IS_ERR(status)) {
                irq_restore(flags);
                return total ? (i64)total : (i64)status;
            }
            continue;
        }
        n = ((len - total) < (u64)space) ? (u32)(len - total) : space;
        tail = (p->head + p->count) % NX_PIPE_CAPACITY;
        first = NX_PIPE_CAPACITY - tail;
        if (first > n)
            first = n;
        status = copy_from_user(p->buf + tail, ubuf + total, first);
        if (!NSTATUS_IS_ERR(status) && n > first)
            status = copy_from_user(p->buf, ubuf + total + first, n - first);
        if (NSTATUS_IS_ERR(status)) {
            irq_restore(flags);
            return total ? (i64)total : (i64)status;
        }
        p->count += n;
        total += n;
        (void)waitq_wake_all(&p->rq);
    }
    irq_restore(flags);
    return (i64)total;
}

u32 pipe_poll_bits(const nx_pipe_t *p, bool write_end)
{
    u32 bits = 0;

    if (write_end) {
        if (p->readers == 0)
            bits |= NX_POLL_HUP;
        else if (p->count < NX_PIPE_CAPACITY)
            bits |= NX_POLL_OUT;
    } else {
        if (p->count > 0)
            bits |= NX_POLL_IN;
        if (p->writers == 0)
            bits |= NX_POLL_IN | NX_POLL_HUP;   /* 읽으면 바로 EOF 를 받는다 */
    }
    return bits;
}

waitq_t *pipe_waitq(nx_pipe_t *p, bool write_end)
{
    return write_end ? &p->wq : &p->rq;
}
