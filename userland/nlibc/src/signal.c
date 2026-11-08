/*
 * signal.c - C89 7.7 + POSIX 신호 (의미는 library/signal.h). 커널 쪽은 플랫폼 계층의 __nl_sig* 가 맡는다.
 */
#include <signal.h>
#include <errno.h>
#include "nl_internal.h"

static int valid(int sig)
{
    return sig > 0 && sig < NSIG;
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    struct __nl_sigact k;
    struct __nl_sigact o;

    if (!valid(sig)) {
        errno = EINVAL;
        return -1;
    }
    if (act) {
        k.handler = (unsigned long)act->sa_handler;
        k.mask = act->sa_mask;
        k.flags = act->sa_flags;
    }
    if (__nl_sigaction(sig, act ? &k : 0, &o) < 0)
        return -1;
    if (old) {
        old->sa_handler = (void (*)(int))o.handler;
        old->sa_mask = o.mask;
        old->sa_flags = o.flags;
    }
    return 0;
}

void (*signal(int sig, void (*func)(int)))(int)
{
    struct sigaction sa;
    struct sigaction old;

    if (func == SIG_ERR) {
        errno = EINVAL;
        return SIG_ERR;
    }
    sa.sa_handler = func;
    sa.sa_mask = 0;
    /* System V: 부르기 전에 SIG_DFL 로 되돌리고 그 신호를 막지 않는다 (C89 7.7.1.1 이 허용하는 방식) */
    sa.sa_flags = (func == SIG_DFL || func == SIG_IGN) ? 0 : (int)(SA_RESETHAND | SA_NODEFER);
    if (sigaction(sig, &sa, &old) < 0)
        return SIG_ERR;
    return old.sa_handler;
}

int raise(int sig)
{
    if (!valid(sig)) {
        errno = EINVAL;
        return -1;
    }
    /* 커널이 자기 자신에게 보낸다: 이 시스템 콜이 돌아가는 길에 처리기가 불리거나 기본 동작으로 끝난다 */
    return __nl_sigsend(-1, sig);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
        errno = EINVAL;
        return -1;
    }
    return __nl_sigprocmask(how, set, old);
}

int sigpending(sigset_t *set)
{
    if (!set) {
        errno = EFAULT;
        return -1;
    }
    return __nl_sigpending(set);
}

int sigsuspend(const sigset_t *mask)
{
    if (!mask) {
        errno = EFAULT;
        return -1;
    }
    return __nl_sigsuspend(mask);
}

int sigemptyset(sigset_t *set)
{
    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set)
{
    *set = (1UL << (NSIG - 1)) - 1UL;
    return 0;
}

int sigaddset(sigset_t *set, int sig)
{
    if (!valid(sig)) {
        errno = EINVAL;
        return -1;
    }
    *set |= 1UL << (sig - 1);
    return 0;
}

int sigdelset(sigset_t *set, int sig)
{
    if (!valid(sig)) {
        errno = EINVAL;
        return -1;
    }
    *set &= ~(1UL << (sig - 1));
    return 0;
}

int sigismember(const sigset_t *set, int sig)
{
    if (!valid(sig)) {
        errno = EINVAL;
        return -1;
    }
    return (*set >> (sig - 1)) & 1UL ? 1 : 0;
}
