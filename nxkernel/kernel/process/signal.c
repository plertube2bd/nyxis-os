/*
 * signal.c - 신호 (설계는 signal.h, 공개 규약은 include/nyx_abi.h)
 */

#include "kernel/process/signal.h"
#include "kernel/process/process.h"
#include "kernel/process/waitq.h"
#include "kernel/process/schedule.h"
#include "kernel/syscall/uaccess.h"
#include "kernel/syscall/syscall_internal.h"
#include "kernel/paging/paging.h"
#include "console/outputs/printk.h"
#include "nyx_abi.h"
#include "memory.h"

NX_STATIC_ASSERT(nx_sigaction_size, sizeof(struct nx_sigaction) == 24);
NX_STATIC_ASSERT(nx_siginfo_size, sizeof(struct nx_siginfo) == 128);
NX_STATIC_ASSERT(nx_sigcontext_size, sizeof(struct nx_sigcontext) == 664);
NX_STATIC_ASSERT(nx_sigframe_size, sizeof(struct nx_sigframe) == 800);

#define RED_ZONE 128UL

/* 사용자가 NxSignalReturn 으로 되돌릴 수 있는 RFLAGS 비트: CF PF AF ZF SF DF OF AC. IF 는 늘 켠다 */
#define RFLAGS_USER_BITS 0x40CD5UL
#define RFLAGS_IF        0x200UL
#define RFLAGS_FIXED     0x2UL
#define RFLAGS_TF        0x100UL
#define RFLAGS_DF        0x400UL

/* 신호 프레임을 만들 커널 쪽 버퍼. 비선점 커널 + 인터럽트를 끈 채로만 쓰므로 하나로 충분하다 */
static struct nx_sigframe g_frame;
static u8 g_fpu[512] __attribute__((aligned(16)));
static u32 g_mxcsr_mask = 0;

static bool valid_sig(u64 sig)
{
    return sig >= 1 && sig < NX_SIG_MAX;
}

/* 기본 동작이 무시인 신호 */
static bool default_ignores(u32 sig)
{
    switch (sig) {
    case NX_SIGCHLD:
    case NX_SIGCONT:
    case NX_SIGURG:
    case NX_SIGWINCH:
    case NX_SIGTSTP:                         /* 멈춤(job control stop)은 지원하지 않는다 */
    case NX_SIGTTIN:
    case NX_SIGTTOU:
        return true;
    default:
        return false;
    }
}

static bool is_ignored(const process_t *p, u32 sig)
{
    u64 h = p->sig_action[sig].handler;

    return (h == NX_SIG_IGN || (h == NX_SIG_DFL && default_ignores(sig))) ? true : false;
}

bool signal_deliverable(const process_t *p)
{
    if (!p || p->sig_hold)
        return false;
    return (p->sig_pending & ~p->sig_mask) ? true : false;
}

/* 잠든 p 를 깨운다 (신호). 인터럽트를 끈 채 호출 */
static void wake_for_signal(process_t *p)
{
    if (p->state != PROCESS_BLOCKED || p->sig_hold)
        return;
    waitq_cancel_locked(p);
    p->sig_woken = true;
    p->wake_tick = 0;
    p->state = PROCESS_READY;
    sched_request_resched();
}

bool signal_protected(const process_t *target, u32 sig, u32 sender_pid)
{
    if (!target || !target->critical)
        return false;
    if (sig == 0 || sig == NX_SIGCHLD || sig == NX_SIGCONT || sig == NX_SIGURG || sig == NX_SIGWINCH)
        return false;
    if (sender_pid != 0 && (sender_pid == target->pid || sender_pid == target->parent_pid))
        return false;
    return true;
}

void signal_post(process_t *p, u32 sig, i32 code, u32 sender)
{
    u64 flags;

    (void)code;
    if (!p || !valid_sig(sig) || sig == NX_SIGKILL || sig == NX_SIGSTOP)
        return;
    if (signal_protected(p, sig, sender))
        return;                         /* 보호 프로세스 */

    flags = irq_save();
    if (p->in_use && p->addrspace.pml4 && p->state != PROCESS_TERMINATED && !is_ignored(p, sig)) {
        p->sig_pending |= SIG_BIT(sig);
        p->sig_sender[sig] = sender;
        if (!(p->sig_mask & SIG_BIT(sig)))
            wake_for_signal(p);
    }
    irq_restore(flags);
}

u32 signal_post_group(u32 pgid, u32 sig, i32 code)
{
    process_t *p;
    u32 n = 0;
    u64 flags;

    if (pgid == 0)
        return 0;
    flags = irq_save();
    for (p = process_list; p; p = p->next) {
        if (p->in_use && p->pid != 0 && p->addrspace.pml4 && p->pgid == pgid &&
            p->state != PROCESS_TERMINATED && !signal_protected(p, sig, 0)) {
            signal_post(p, sig, code, 0);
            n++;
        }
    }
    irq_restore(flags);
    return n;
}

/* ---------------- 프레임 ---------------- */

static u32 mxcsr_mask(void)
{
    if (g_mxcsr_mask == 0) {
        u32 m;

        __asm__ volatile ("fxsave64 (%0)" : : "r"(g_fpu) : "memory");
        memcpy(&m, g_fpu + 28, sizeof(m));
        g_mxcsr_mask = m ? m : 0xFFBFU;      /* 0 이면 CPU 가 기본값(0xFFBF)을 쓴다 */
    }
    return g_mxcsr_mask;
}

/*
 * 현재 프로세스의 사용자 스택에 sig 의 프레임을 쌓고 frame 을 처리기 호출로 바꾼다.
 * 스택에 쓸 수 없으면 false (호출자가 프로세스를 끝낸다).
 */
static bool setup_frame(struct trap_frame *f, u32 sig, i32 code, u32 sender, u64 addr)
{
    process_t *p = current_process;
    u64 sp;
    u32 restore_mask;

    if (f->rsp < RED_ZONE + sizeof(struct nx_sigframe) + 4096UL || f->rsp > USER_SPACE_END)
        return false;
    sp = ((f->rsp - RED_ZONE - sizeof(struct nx_sigframe)) & ~15UL) - 8UL;   /* sp % 16 == 8 */

    memset(&g_frame, 0, sizeof(g_frame));
    g_frame.ret = p->sig_action[sig].restorer;
    g_frame.ctx.r15 = f->r15; g_frame.ctx.r14 = f->r14; g_frame.ctx.r13 = f->r13; g_frame.ctx.r12 = f->r12;
    g_frame.ctx.r11 = f->r11; g_frame.ctx.r10 = f->r10; g_frame.ctx.r9 = f->r9;   g_frame.ctx.r8 = f->r8;
    g_frame.ctx.rbp = f->rbp; g_frame.ctx.rdi = f->rdi; g_frame.ctx.rsi = f->rsi; g_frame.ctx.rdx = f->rdx;
    g_frame.ctx.rcx = f->rcx; g_frame.ctx.rbx = f->rbx; g_frame.ctx.rax = f->rax;
    g_frame.ctx.rip = f->rip;
    g_frame.ctx.rflags = f->rflags;
    g_frame.ctx.rsp = f->rsp;
    restore_mask = p->sig_suspended ? p->sig_suspend_mask : p->sig_mask;
    g_frame.ctx.mask = restore_mask;
    /* 사용자 FPU 레지스터는 지금 하드웨어에 있다 (커널은 FPU 를 쓰지 않는다) */
    __asm__ volatile ("fxsave64 (%0)" : : "r"(g_fpu) : "memory");
    memcpy(g_frame.ctx.fpu, g_fpu, sizeof(g_fpu));

    g_frame.info.signo = (i32)sig;
    g_frame.info.code = code;
    if (code == NX_SI_FAULT && (sig == NX_SIGSEGV || sig == NX_SIGFPE || sig == NX_SIGILL ||
                                sig == NX_SIGTRAP || sig == NX_SIGBUS)) {
        g_frame.info.u.addr = addr;
    } else {
        g_frame.info.u.proc.pid = sender;
    }

    if (NSTATUS_IS_ERR(copy_to_user(sp, &g_frame, sizeof(g_frame))))
        return false;

    /* 처리기 호출: handler(sig, &info, &ctx). 반환 주소 = restorer */
    f->rip = p->sig_action[sig].handler;
    f->rsp = sp;
    f->rdi = sig;
    f->rsi = sp + (u64)((u8 *)&g_frame.info - (u8 *)&g_frame);
    f->rdx = sp + (u64)((u8 *)&g_frame.ctx - (u8 *)&g_frame);
    f->rax = 0;
    f->rflags = (f->rflags & ~(RFLAGS_DF | RFLAGS_TF)) | RFLAGS_IF | RFLAGS_FIXED;

    p->sig_suspended = false;
    p->sig_mask = restore_mask | p->sig_action[sig].mask;
    if (!(p->sig_action[sig].flags & NX_SA_NODEFER))
        p->sig_mask |= SIG_BIT(sig);
    p->sig_mask &= ~SIG_UNBLOCKABLE & SIG_ALL;
    if (p->sig_action[sig].flags & NX_SA_RESETHAND) {
        p->sig_action[sig].handler = NX_SIG_DFL;
        p->sig_action[sig].restorer = 0;
    }
    return true;
}

void signal_deliver(struct trap_frame *f)
{
    process_t *p = current_process;
    u64 flags;

    if (!p || !p->addrspace.pml4 || (f->cs & 3UL) != 3UL)
        return;

    flags = irq_save();
    while (signal_deliverable(p)) {
        u32 ready = p->sig_pending & ~p->sig_mask;
        u32 sig = 1;
        u64 h;

        while (!(ready & SIG_BIT(sig)))
            sig++;
        p->sig_pending &= ~SIG_BIT(sig);
        h = p->sig_action[sig].handler;

        if (h == NX_SIG_IGN || (h == NX_SIG_DFL && default_ignores(sig)))
            continue;
        if (h == NX_SIG_DFL)
            process_exit_with_code(NX_EXIT_SIGNAL(sig));      /* 돌아오지 않는다 */
        if (!setup_frame(f, sig, sig == NX_SIGCHLD ? NX_SI_CHILD : (p->sig_sender[sig] ? NX_SI_USER : NX_SI_KERNEL),
                         p->sig_sender[sig], 0)) {
            printk("signal: cannot push a signal frame (bad user stack), terminating process %u\n", p->pid);
            process_exit_with_code((i32)NX_EXIT_FAULT);
        }
        break;                                   /* 한 번에 처리기 하나: 나머지는 처리기가 돌아온 뒤 */
    }
    if (p->sig_suspended) {
        /* NxSignalSuspend 를 깨운 신호가 처리기 없이 끝났다 (그 사이 무시로 바뀜): 막은 집합을 되돌린다 */
        p->sig_mask = p->sig_suspend_mask;
        p->sig_suspended = false;
    }
    irq_restore(flags);
}

bool signal_fault(struct trap_frame *f, u32 sig, u64 addr)
{
    process_t *p = current_process;
    u64 h;

    if (!p || !p->addrspace.pml4 || !valid_sig(sig))
        return false;
    h = p->sig_action[sig].handler;
    if (h == NX_SIG_DFL || h == NX_SIG_IGN || (p->sig_mask & SIG_BIT(sig)))
        return false;                            /* 처리기 안에서 또 같은 예외: 끝낸다 */
    return setup_frame(f, sig, NX_SI_FAULT, 0, addr);
}

void signal_exec_reset(process_t *p)
{
    u32 i;

    for (i = 1; i < NX_SIG_MAX; i++) {
        if (p->sig_action[i].handler != NX_SIG_IGN) {
            p->sig_action[i].handler = NX_SIG_DFL;
            p->sig_action[i].restorer = 0;
            p->sig_action[i].mask = 0;
            p->sig_action[i].flags = 0;
        }
    }
}

void signal_fork_copy(process_t *child, const process_t *parent)
{
    memcpy(child->sig_action, parent->sig_action, sizeof(child->sig_action));
    child->sig_mask = parent->sig_mask;
    child->sig_pending = 0;
}

/* ---------------- 시스템 콜 ---------------- */

/* NxSignalAction(sig, act, old) */
i64 sys_signal_action(const u64 *a)
{
    process_t *p = current_process;
    struct nx_sigaction act;
    struct nx_sigaction old;
    u32 sig = (u32)a[0];
    Nstatus status;
    u64 flags;

    if (!p)
        return (i64)NnotInitialized;
    if (!valid_sig(a[0]))
        return (i64)NinvalidArg;
    if (a[1]) {
        status = copy_from_user(&act, a[1], sizeof(act));
        if (NSTATUS_IS_ERR(status))
            return (i64)status;
        if (sig == NX_SIGKILL || sig == NX_SIGSTOP)
            return (i64)NinvalidArg;
        if (act.handler != NX_SIG_DFL && act.handler != NX_SIG_IGN) {
            /* 처리기와 돌아갈 곳은 사용자 영역이어야 한다 (iretq 가 커널 모드에서 #GP 를 내지 않도록) */
            if (act.handler >= USER_SPACE_END || act.restorer == 0 || act.restorer >= USER_SPACE_END)
                return (i64)NinvalidArg;
        }
        if (act.flags & ~(u32)(NX_SA_SIGINFO | NX_SA_NODEFER | NX_SA_RESETHAND))
            return (i64)NinvalidArg;
    }

    flags = irq_save();
    old.handler = p->sig_action[sig].handler;
    old.restorer = p->sig_action[sig].restorer;
    old.mask = p->sig_action[sig].mask;
    old.flags = p->sig_action[sig].flags;
    if (a[1]) {
        p->sig_action[sig].handler = act.handler;
        p->sig_action[sig].restorer = (act.handler > NX_SIG_IGN) ? act.restorer : 0;
        p->sig_action[sig].mask = act.mask & SIG_ALL & ~SIG_UNBLOCKABLE;
        p->sig_action[sig].flags = act.flags;
        if (is_ignored(p, sig))
            p->sig_pending &= ~SIG_BIT(sig);    /* 무시로 바꾸면 기다리던 것도 버린다 (POSIX) */
    }
    irq_restore(flags);

    if (a[2])
        return (i64)copy_to_user(a[2], &old, sizeof(old));
    return 0;
}

/* NxSignalMask(how, set) -> 이전 집합 */
i64 sys_signal_mask(const u64 *a)
{
    process_t *p = current_process;
    u32 set = (u32)a[1] & SIG_ALL & ~SIG_UNBLOCKABLE;
    u32 old;
    u64 flags;

    if (!p)
        return (i64)NnotInitialized;
    if (a[0] > NX_SIG_SETMASK)
        return (i64)NinvalidArg;
    flags = irq_save();
    old = p->sig_mask;
    if (a[0] == NX_SIG_BLOCK)
        p->sig_mask |= set;
    else if (a[0] == NX_SIG_UNBLOCK)
        p->sig_mask &= ~set;
    else
        p->sig_mask = set;
    irq_restore(flags);
    return (i64)old;            /* 막힌 신호가 풀렸으면 이 시스템 콜이 돌아가는 길에 전달된다 */
}

/* NxSignalSend(target, sig) */
i64 sys_signal_send(const u64 *a)
{
    process_t *p = current_process;
    process_t *target = p;

    if (!p)
        return (i64)NnotInitialized;
    if (a[1] != 0 && !valid_sig(a[1]))
        return (i64)NinvalidArg;
    if (a[0] != NX_PID_SELF) {
        Nstatus status = lookup_process(a[0], (u32)NX_RIGHT_KILL, &target);

        if (NSTATUS_IS_ERR(status))
            return (i64)status;
    }
    if (signal_protected(target, (u32)a[1], p->pid))
        return (i64)Npermission;            /* 보호 프로세스 (init, login) */
    if (a[1] == 0)
        return 0;
    if (a[1] == NX_SIGKILL || a[1] == NX_SIGSTOP) {
        /* 멈춤은 지원하지 않는다: SIGSTOP 도 종료로 다룬다 (가로챌 수 없는 신호의 뜻은 지킨다) */
        return (i64)process_kill(target, (i32)NX_EXIT_KILLED);   /* 자기 자신이면 돌아오지 않는다 */
    }
    signal_post(target, (u32)a[1], NX_SI_USER, p->pid);
    return 0;
}

/* NxSignalSendPid(pid, sig): root 는 누구에게나, 그 밖에는 같은 uid 의 사용자 프로세스에게만 */
i64 sys_signal_send_pid(const u64 *a)
{
    process_t *p = current_process;
    process_t *target = nNULL;
    process_t *q;

    if (!p)
        return (i64)NnotInitialized;
    if (a[1] != 0 && !valid_sig(a[1]))
        return (i64)NinvalidArg;
    if (a[0] > 0xFFFFFFFFUL)
        return (i64)NinvalidArg;
    for (q = process_list; q; q = q->next) {
        if (q->in_use && q->state != PROCESS_TERMINATED && q->pid == (u32)a[0]) {
            target = q;
            break;
        }
    }
    if (!target)
        return (i64)NnotFound;
    if (!target->addrspace.pml4)
        return (i64)Npermission;            /* 커널 스레드 */
    if (p->uid != 0 && target->uid != p->uid)
        return (i64)Npermission;
    if (signal_protected(target, (u32)a[1], p->pid))
        return (i64)Npermission;            /* 보호 프로세스 (init, login): root 도 */
    if (a[1] == 0)
        return 0;
    if (a[1] == NX_SIGKILL || a[1] == NX_SIGSTOP)
        return (i64)process_kill(target, (i32)NX_EXIT_KILLED);   /* 자기 자신이면 돌아오지 않는다 */
    signal_post(target, (u32)a[1], NX_SI_USER, p->pid);
    return 0;
}

/* NxSignalReturn(): 사용자 rsp 가 nx_sigcontext 를 가리킨다 (처리기의 ret 가 nx_sigframe.ret 을 꺼냈다) */
i64 sys_signal_return(const u64 *a)
{
    process_t *p = current_process;
    struct trap_frame *f;
    struct nx_sigcontext *c = &g_frame.ctx;
    u32 mm = mxcsr_mask();                  /* 먼저: 처음 부를 때 g_fpu 를 쓴다 */
    u32 mx;
    u64 flags;

    (void)a;
    if (!p || !p->uframe)
        return (i64)NnotInitialized;
    f = p->uframe;

    if (NSTATUS_IS_ERR(copy_from_user(c, f->rsp, sizeof(*c))) ||
        c->rip >= USER_SPACE_END || c->rsp >= USER_SPACE_END) {
        printk("signal: bad signal frame in NxSignalReturn, terminating process %u\n", p->pid);
        process_exit_with_code((i32)NX_EXIT_FAULT);
    }

    flags = irq_save();
    f->r15 = c->r15; f->r14 = c->r14; f->r13 = c->r13; f->r12 = c->r12;
    f->r11 = c->r11; f->r10 = c->r10; f->r9 = c->r9;   f->r8 = c->r8;
    f->rbp = c->rbp; f->rdi = c->rdi; f->rsi = c->rsi; f->rdx = c->rdx;
    f->rcx = c->rcx; f->rbx = c->rbx;
    f->rip = c->rip;
    f->rsp = c->rsp;
    f->rflags = (c->rflags & RFLAGS_USER_BITS) | RFLAGS_IF | RFLAGS_FIXED;
    f->cs = SEL_USER_CODE;
    f->ss = SEL_USER_DATA;
    p->sig_mask = c->mask & SIG_ALL & ~SIG_UNBLOCKABLE;

    /* FPU: 예약된 MXCSR 비트가 켜져 있으면 fxrstor 가 커널 모드 #GP 를 낸다 -> 지운다 */
    memcpy(g_fpu, c->fpu, sizeof(g_fpu));
    memcpy(&mx, g_fpu + 24, sizeof(mx));
    mx &= mm;
    memcpy(g_fpu + 24, &mx, sizeof(mx));
    __asm__ volatile ("fxrstor64 (%0)" : : "r"(g_fpu) : "memory");

    p->sig_iret = true;                     /* rcx/r11 까지 되돌리려면 sysret 이 아니라 iretq 로 */
    irq_restore(flags);
    return (i64)c->rax;                     /* syscall_handle 이 frame->rax 에 넣는다 */
}

/* NxSignalSuspend(set) */
i64 sys_signal_suspend(const u64 *a)
{
    process_t *p = current_process;
    waitq_t q;
    u64 flags;

    if (!p)
        return (i64)NnotInitialized;
    flags = irq_save();
    p->sig_suspend_mask = p->sig_mask;
    p->sig_suspended = true;
    p->sig_mask = (u32)a[0] & SIG_ALL & ~SIG_UNBLOCKABLE;
    waitq_init(&q);
    while (!signal_deliverable(p)) {
        Nstatus status = waitq_sleep_locked(&q, 0);   /* 아무도 이 큐를 깨우지 않는다: 신호만 깨운다 */

        if (NSTATUS_IS_ERR(status) && status != Ninterrupted)
            break;
    }
    irq_restore(flags);
    return (i64)Ninterrupted;               /* 돌아가는 길에 전달하고, 처리기가 끝나면 원래 집합으로 */
}

/* NxSignalPending() */
i64 sys_signal_pending(const u64 *a)
{
    process_t *p = current_process;

    (void)a;
    if (!p)
        return (i64)NnotInitialized;
    return (i64)(p->sig_pending & p->sig_mask);
}
