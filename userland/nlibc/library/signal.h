/*
 * signal.h - C89 7.7 + POSIX 신호
 *
 * 커널 신호(include/nyx_abi.h 의 "신호")를 쓴다: 처리기는 신호가 온 프로세스가 사용자 모드로 돌아갈 때 불린다.
 *   - 보내는 곳: raise/kill(자기 자신이나 fork/posix_spawn 으로 만든 자식), 콘솔(Ctrl-C = SIGINT, Ctrl-\ = SIGQUIT,
 *     포그라운드 그룹에: unistd.h 의 tcsetpgrp), CPU 예외(SIGSEGV SIGFPE SIGILL SIGTRAP), 자식의 종료(SIGCHLD).
 *   - CPU 예외는 처리기가 있고 막히지 않았을 때만 처리기로 간다. 처리기가 돌아오면 같은 명령을 다시 실행하므로
 *     보통 longjmp 나 _exit 로 빠져나간다. 처리기가 없으면 커널이 끝낸다 (waitpid 에는 SIGSEGV).
 *   - 신호가 잠든 호출(read, waitpid, sleep, poll ...)을 깨우면 그 호출은 -1 + EINTR 로 돌아온다. SA_RESTART 는
 *     받아 두지만 다시 시작하지 않는다.
 *   - SIGKILL 은 가로채거나 막을 수 없다. 멈춤은 지원하지 않는다: SIGSTOP 은 SIGKILL 처럼 끝내고,
 *     SIGTSTP/SIGTTIN/SIGTTOU 의 기본 동작은 무시.
 *   - signal() 은 System V 방식이다: 처리기를 부르기 직전에 SIG_DFL 로 되돌리고 그 신호를 막지 않는다
 *     (계속 받으려면 sigaction 을 쓴다).
 */
#ifndef _NL_SIGNAL_H
#define _NL_SIGNAL_H

typedef int sig_atomic_t;

#define SIG_DFL ((void (*)(int))0)
#define SIG_ERR ((void (*)(int))-1)
#define SIG_IGN ((void (*)(int))1)

/* 번호는 흔한 관례(POSIX, Linux x86_64)와 같다 */
#define SIGINT   2
#define SIGILL   4
#define SIGABRT  6
#define SIGFPE   8
#define SIGSEGV  11
#define SIGTERM  15

void (*signal(int sig, void (*func)(int)))(int);
int raise(int sig);

#include "_nl_features.h"
#if _NL_POSIX
#define _NL_WANT_PID_T
#define _NL_WANT_ID_T
#include "_nl_types.h"

#define SIGHUP    1
#define SIGQUIT   3
#define SIGTRAP   5
#define SIGIOT    SIGABRT
#define SIGBUS    7
#define SIGKILL   9
#define SIGUSR1   10
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGWINCH  28   /* 창 크기 바뀜 (pty 의 크기가 바뀌면 포그라운드 그룹에. 물리 콘솔은 바뀌지 않는다) */
#define NSIG      32

typedef unsigned long sigset_t;    /* 비트 (sig - 1) */

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    int __pad0;
    union {
        struct { pid_t __pid; uid_t __uid; } __k;
        void *__addr;
    } __u;
    char __reserved[104];
} siginfo_t;
#define si_pid  __u.__k.__pid
#define si_uid  __u.__k.__uid
#define si_addr __u.__addr

/* si_code */
#define SI_USER      0          /* kill/raise */
#define SI_KERNEL    0x80       /* 콘솔 (Ctrl-C 등) */
#define SEGV_MAPERR  1          /* CPU 예외 (si_addr = 잘못된 주소) */
#define FPE_INTDIV   1
#define ILL_ILLOPC   1
#define CLD_EXITED   1          /* SIGCHLD (si_pid = 끝난 자식) */

struct sigaction {
    union {
        void (*__h)(int);
        void (*__a)(int, siginfo_t *, void *);
    } __sa_u;
    sigset_t sa_mask;
    int sa_flags;
};
#define sa_handler   __sa_u.__h
#define sa_sigaction __sa_u.__a

/* sa_flags (값은 Linux 와 같다) */
#define SA_SIGINFO   0x00000004
#define SA_RESTART   0x10000000  /* 받아 두지만 효과 없음 (호출은 EINTR 로 돌아온다) */
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

/* sigprocmask 의 how */
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
int sigprocmask(int how, const sigset_t *set, sigset_t *old);
int sigpending(sigset_t *set);
int sigsuspend(const sigset_t *mask);   /* 늘 -1 + EINTR (처리기가 돌아온 뒤) */
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);

/* 이 프로세스 또는 fork/posix_spawn 으로 만든 자식에게만 보낼 수 있다 (그 밖은 ESRCH). sig 0 은 확인만 */
int kill(pid_t pid, int sig);
#endif

#endif /* _NL_SIGNAL_H */
