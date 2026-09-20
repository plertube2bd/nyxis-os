/*
 * syscall.h - 시스템 콜 (int 0x80)
 *
 * ABI (syscalls.txt 참고):
 *   rax = 시스템 콜 번호
 *   rdi, rsi, rdx, r10, r8, r9 = 인자 0~5
 *   반환: rax >= 0 성공, rax < 0 오류 (Nstatus 를 부호 확장한 값)
 */
#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include "nyxis.h"
#include "interrupt.h"

/* 시스템 콜 번호 */
#define NX_SYS_KERNEL_PRINT  771U   /* NxKernelPrint(const char *user_string) */
#define NX_SYS_DEBUG_NOP     777U   /* NxDebugNop() -> 0 */

/* user 문자열 한 번에 커널로 복사할 최대 길이 (NUL 포함) */
#define SYSCALL_STRING_MAX   256U

/* 트랩 프레임에서 인자를 꺼내 처리하고 결과를 frame->rax 에 쓴다 */
void syscall_handle(struct trap_frame *frame);

/* 실제 처리 함수 (호스트 테스트/재사용을 위해 분리). 반환값은 부호 확장된 Nstatus. */
i64 syscall_dispatch(
    u64 syscall_nr,
    u64 a0,
    u64 a1,
    u64 a2,
    u64 a3,
    u64 a4,
    u64 a5
);

#endif /* KERNEL_SYSCALL_H */
