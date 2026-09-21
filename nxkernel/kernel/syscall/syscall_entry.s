/*
 * syscall_entry.s - `syscall` 명령 진입점
 *
 * 하드웨어가 하는 일: rcx = 복귀 RIP, r11 = RFLAGS, CS/SS = 커널 셀렉터, RFLAGS &= ~SFMASK.
 * 스택은 전환되지 않으므로 여기서 직접 커널 스택으로 바꿔야 한다. (rsp 는 아직 "사용자 rsp")
 *
 * 처리 순서:
 *   1) swapgs 로 GS.base 를 g_cpu_local 로 전환 -> 사용자 rsp 보관 -> 커널 스택 로드
 *   2) 인터럽트 진입과 같은 struct trap_frame 을 만들어 syscall_handle() 호출 (int 0x80 과 동일한 처리 코드)
 *   3) 복귀 RIP 이 사용자 영역의 정규 주소인지 검사한 뒤 sysretq
 *      (비정규 RIP 로 sysret 하면 Intel CPU 에서 "커널 모드 #GP" 가 사용자 스택 위에서 발생하는 알려진 취약점)
 *
 * 오프셋: struct cpu_local { u64 kernel_rsp (0); u64 user_rsp (8); }
 *         struct trap_frame 오프셋: vector 120, error 128, rip 136, cs 144, rflags 152, rsp 160, ss 168
 */


    .section .text
    .global syscall_entry
    .type syscall_entry, @function
syscall_entry:
    swapgs
    movq %rsp, %gs:8                /* 사용자 rsp 보관 */
    movq %gs:0, %rsp                /* 커널 스택으로 전환 */

    pushq $0x23                     /* ss  = SEL_USER_DATA */
    pushq %gs:8                     /* rsp = 사용자 rsp */
    pushq %r11                      /* rflags */
    pushq $0x2B                     /* cs  = SEL_USER_CODE (64비트) */
    pushq %rcx                      /* rip */
    pushq $0                        /* error_code */
    pushq $0x80                     /* vector (int 0x80 과 같은 시스템 콜 벡터로 표기) */

    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    cld
    movq %rsp, %rdi                 /* 인자: struct trap_frame * */
    call syscall_handle

    /* 복귀 RIP 검사: 사용자 영역(< 0x0000800000000000)이어야 한다 (정규성도 함께 보장됨) */
    movq 136(%rsp), %rax
    shrq $47, %rax
    jnz syscall_bad_return_stub

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    /* 이 시점 rsp -> vector 슬롯. sysret 용 rcx/r11/rsp 를 프레임에서 로드 (rcx, r11 은 규약상 파괴됨) */
    movq 16(%rsp), %rcx             /* rip */
    movq 32(%rsp), %r11             /* rflags */
    movq 40(%rsp), %rsp             /* 사용자 rsp */
    swapgs
    sysretq

syscall_bad_return_stub:
    swapgs                          /* C 코드는 GS 를 쓰지 않으므로 평상시 상태로 되돌림 */
    call syscall_bad_return
1:  hlt
    jmp 1b
    .size syscall_entry, . - syscall_entry

    .section .note.GNU-stack,"",@progbits
