/*
 * nyxstd.h - Nyxis 유저랜드 시스템 콜 래퍼 (x86_64, int 0x80)
 *
 * ABI (nxkernel/kernel/syscall/syscalls.txt):
 *   rax = 번호, rdi/rsi/rdx/r10/r8/r9 = 인자 0~5, 반환은 rax (음수이면 오류)
 *
 * [수정 이력 요약]
 *  - 기존 인라인 asm 은 모든 값을 "r" (아무 레지스터) 제약으로 받은 뒤 asm 안에서
 *    mov 로 rax/rdi/rsi... 에 옮겼다. 컴파일러가 입력값(예: rdx 용 값)을 이미 rdi 등에
 *    배정해 두었다면 앞선 mov 가 그 값을 덮어써 인자가 뒤섞인다 (최적화 수준에 따라 발생).
 *    -> 특정 레지스터에 직접 바인딩하는 제약("a","D","S","d" + register 변수)으로 교체.
 *  - 32비트 분기 삭제: 이 OS 는 x86_64 전용이고, 32비트 분기는 int 0x80 대신 syscall
 *    명령을 쓰는 등 ABI 도 맞지 않았다.
 *  - C89 호환: long long -> unsigned long(LP64), inline -> __inline__.
 */
#ifndef NYXSTD_H
#define NYXSTD_H

#include <stdint.h>

#if INTPTR_MAX != 0x7FFFFFFFFFFFFFFFL
#error "nyxstd.h supports x86_64 only"
#endif

#define _NYX64

typedef unsigned long nyx_u64;

static __inline__ nyx_u64 syscall_wrapper(nyx_u64 sysno, nyx_u64 a0, nyx_u64 a1,
                                          nyx_u64 a2, nyx_u64 a3, nyx_u64 a4, nyx_u64 a5)
{
    nyx_u64 ret;
    register nyx_u64 r10_ __asm__("r10") = a3;
    register nyx_u64 r8_  __asm__("r8")  = a4;
    register nyx_u64 r9_  __asm__("r9")  = a5;

    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(sysno), "D"(a0), "S"(a1), "d"(a2), "r"(r10_), "r"(r8_), "r"(r9_)
        : "rcx", "r11", "memory"
    );
    return ret;
}

#endif /* NYXSTD_H */
