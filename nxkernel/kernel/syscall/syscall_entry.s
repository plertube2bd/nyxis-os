/*
 * syscall_entry.s - `syscall` 명령 진입점
 *
 * 하드웨어가 하는 일: rcx = 복귀 RIP, r11 = RFLAGS, CS/SS = 커널 셀렉터, RFLAGS &= ~SFMASK.
 * 스택은 전환되지 않으므로 여기서 직접 커널 스택으로 바꿔야 한다. (rsp 는 아직 "사용자 rsp")
 *
 * 처리 순서:
 *   1) swapgs 로 GS.base 를 g_cpu_local 로 전환 -> 사용자 rsp 보관 -> 커널 스택 로드
 *      -> 사용자 rsp 를 프레임에 넣은 즉시 swapgs 로 GS 를 "평상시 상태"로 되돌린다
 *   2) 인터럽트 진입과 같은 struct trap_frame 을 만들어 syscall_handle() 호출 (int 0x80 과 동일한 처리 코드)
 *   3) 복귀 RIP 이 사용자 영역의 정규 주소인지 검사한 뒤 sysretq
 *      (비정규 RIP 로 sysret 하면 Intel CPU 에서 "커널 모드 #GP" 가 사용자 스택 위에서 발생하는 알려진 취약점)
 *
 * [GS 규칙] GS 가 뒤바뀐(swapped) 상태는 이 파일의 진입부 몇 명령 안에서만 존재한다.
 *   예전에는 swapped 상태로 C 코드를 실행하고 sysret 직전에 되돌렸다. 그런데 NxProcessExit/NxSleep/
 *   NxYield 는 C 코드 안에서 schedule() 로 다른 프로세스로 넘어가고, 그 프로세스는 iretq(인터럽트/
 *   int 0x80/enter_ring3)로 ring3 에 돌아가므로 swapgs 가 짝을 이루지 못했다. 그 결과 다음 `syscall`
 *   에서 GS.base 가 0 이 되어 "movq %rsp, %gs:8" 이 주소 0x8 에 쓰다가, 사용자 rsp 위에서 커널 모드
 *   #PF 가 났다 (유저가 rsp 를 커널 주소로 맞추면 커널 메모리에 예외 프레임이 써지는 보안 문제).
 *   진입 직후 되돌리면 C 코드/스케줄러/ISR 은 항상 같은 GS 상태만 보게 된다.
 *   (SFMASK 가 IF 를 끄므로 swapped 구간에 IRQ 는 들어오지 않는다. NMI/#MC 는 IST 스택을 쓰고 GS 를 쓰지 않는다)
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
    swapgs                          /* GS 를 평상시 상태로 (위 [GS 규칙] 참고). 이후 %gs 를 쓰지 않는다 */
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
    testq %rax, %rax
    jnz syscall_iret_exit           /* NxSignalReturn: 모든 레지스터(rcx/r11 포함)를 그대로 되돌린다 */

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
    sysretq

/* iretq 로 복귀: 프레임 전체 복원 (signal.c 가 rip/rsp 는 사용자 영역, cs/ss 는 사용자 셀렉터로 맞춰 두었다) */
syscall_iret_exit:
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
    addq $16, %rsp                  /* vector, error_code */
    iretq
syscall_bad_return_stub:
    call syscall_bad_return
1:  hlt
    jmp 1b
    .size syscall_entry, . - syscall_entry

    .section .note.GNU-stack,"",@progbits
