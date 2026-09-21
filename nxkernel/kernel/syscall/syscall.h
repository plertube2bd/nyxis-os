/*
 * syscall.h - 시스템 콜 (int 0x80)
 *
 * 두 가지 진입 방법을 모두 지원하며 레지스터 규약은 같다 (syscalls.txt 참고):
 *   - int 0x80          (DPL3 인터럽트 게이트, 벡터 0x80)
 *   - syscall / sysret  (MSR: EFER.SCE, STAR, LSTAR, SFMASK. 진입은 syscall_entry.s)
 *   rax = 시스템 콜 번호
 *   rdi, rsi, rdx, r10, r8, r9 = 인자 0~5
 *   반환: rax >= 0 성공, rax < 0 오류 (Nstatus 를 부호 확장한 값)
 *   syscall 명령은 rcx/r11 을 파괴한다 (하드웨어 규약).
 */
#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include "nyxis.h"
#include "interrupt.h"

#include "nyx_abi.h"   /* 시스템 콜 번호/구조체/상수 (유저랜드와 공유) */

/* user 문자열 한 번에 커널로 복사할 최대 길이 (NUL 포함) */
#define SYSCALL_STRING_MAX   256U

/* CPU 별 데이터. syscall_entry.s 가 GS 로 접근한다 (오프셋 0, 8 은 asm 과 일치해야 함). */
struct cpu_local {
    u64 kernel_rsp;   /* syscall 진입 시 전환할 커널 스택 최상단 */
    u64 user_rsp;     /* syscall 진입 시 사용자 rsp 임시 보관 */
};

extern struct cpu_local g_cpu_local;

/* syscall/sysret 활성화 (MSR 설정). gdt_init() 이후에 호출. CPU 가 지원하지 않으면 NdeviceMissing. */
Nstatus syscall_init(void);

/* syscall_entry.s 가 호출하는 진입점 */
void syscall_entry(void);

/* sysret 로 돌아갈 수 없는 RIP(비정규/커널 영역)일 때 asm 이 호출: 프로세스를 종료한다 */
void syscall_bad_return(void) __attribute__((noreturn));

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
