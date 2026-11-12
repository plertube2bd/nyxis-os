/*
 * signal.h - 신호: 프로세스에 비동기로 알리고, 사용자 처리기가 가로챌 수 있게 한다
 *
 * 공개 규약(번호, 시스템 콜, 스택 모양)은 include/nyx_abi.h 의 "신호" 에 있다. 여기는 커널 안의 쓰임새.
 *
 * [보내기] signal_post() 는 대기 집합에 비트를 켜고, 막히지 않았으면 잠든 대상을 깨운다 (IRQ 핸들러에서도 안전).
 *   지금 무시되는 신호(NX_SIG_IGN, 또는 기본 동작이 무시)는 보낼 때 버린다. SIGKILL 은 여기로 오지 않는다:
 *   호출하는 쪽이 process_kill 로 바로 끝낸다 (시스템 콜 문맥에서만).
 *
 * [전달] signal_deliver() 는 사용자 모드로 돌아가기 직전에 현재 프로세스의 프레임으로 부른다:
 *   - 시스템 콜 끝 (syscall_handle), 결과를 frame->rax 에 넣은 뒤
 *   - 사용자 모드로 돌아가는 IRQ 의 EOI 와 선점 지점 뒤 (interrupt.c)
 *   기본 동작이 종료면 여기서 process_exit 하므로 돌아오지 않는다. 처리기면 사용자 스택에 nx_sigframe 을 쌓고
 *   frame 의 rip/rsp/rdi/rsi/rdx 를 처리기 호출 모양으로 바꾼다.
 *
 * [잠과 깨움] 잠들기 전에 전달할 신호가 있으면 바로 Ninterrupted (waitq_sleep, process_sleep_ticks).
 *   잠든 동안 신호가 오면 깨우고 sig_woken 을 켜서 같은 값을 돌려준다. sig_hold 인 잠은 깨우지 않는다.
 */
#ifndef KERNEL_PROCESS_SIGNAL_H
#define KERNEL_PROCESS_SIGNAL_H

#include "nyxis.h"
#include "interrupt.h"

struct process;

#define SIG_BIT(sig) (1U << ((sig) - 1U))
/* 막을 수도, 가로챌 수도 없는 신호 */
#define SIG_UNBLOCKABLE (SIG_BIT(NX_SIGKILL) | SIG_BIT(NX_SIGSTOP))
#define SIG_ALL ((u32)0x7FFFFFFFU)          /* 1..31 */

/* p 에 sig 를 보낸다. sender 는 보낸 pid (커널이면 0, SIGCHLD 면 끝난 자식) 로 nx_siginfo 에 쓰인다.
 * code 는 지금 쓰지 않는다 (전달할 때 신호와 sender 로 정한다) */
void signal_post(struct process *p, u32 sig, i32 code, u32 sender);

/*
 * 보호 프로세스(process->critical)에 sender_pid 가 sig 를 보내면 안 되면 true. 막지 않는 것: 확인(sig 0), 해가 없는 신호
 * (SIGCHLD SIGCONT SIGURG SIGWINCH), 자기 자신, 부모(init -> login). 커널이 보내는 신호(sender 0: Ctrl-C 등)는 막는다.
 * signal_post 는 막힌 신호를 조용히 버리고, 시스템 콜(NxSignalSend/SendPid/ProcessKill)은 Npermission 을 돌려준다.
 */
bool signal_protected(const struct process *target, u32 sig, u32 sender_pid);

/* 그룹 pgid 의 살아 있는 사용자 프로세스 모두에게 보낸다. 보낸(살아 있던) 프로세스 수 */
u32 signal_post_group(u32 pgid, u32 sig, i32 code);

/* 지금 전달할 신호(대기 중이고 막히지 않음)가 있는지. sig_hold 면 false */
bool signal_deliverable(const struct process *p);

/* 위 [전달]. frame 은 사용자 모드로 돌아갈 프레임 */
void signal_deliver(struct trap_frame *frame);

/*
 * 사용자 모드 CPU 예외를 신호로. 처리기가 있고 막히지 않았으면 처리기로 보내고 true.
 * 아니면 false (호출자가 NX_EXIT_FAULT 로 끝낸다). addr 은 잘못된 주소.
 */
bool signal_fault(struct trap_frame *frame, u32 sig, u64 addr);

/* exec: 함수 처리기를 기본으로 되돌린다 (무시와 막은 집합, 대기 신호는 유지) */
void signal_exec_reset(struct process *p);

/* fork: 처리기와 막은 집합을 물려준다 (대기 신호는 물려주지 않는다) */
void signal_fork_copy(struct process *child, const struct process *parent);

/* 시스템 콜 (nyx_abi.h). a 는 시스템 콜 인자 */
i64 sys_signal_action(const u64 *a);
i64 sys_signal_mask(const u64 *a);
i64 sys_signal_send(const u64 *a);
i64 sys_signal_send_pid(const u64 *a);
i64 sys_signal_return(const u64 *a);
i64 sys_signal_suspend(const u64 *a);
i64 sys_signal_pending(const u64 *a);

#endif /* KERNEL_PROCESS_SIGNAL_H */
