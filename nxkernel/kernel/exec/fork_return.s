/*
 * fork_return.s - fork 로 만든 자식이 처음으로 사용자 모드에 들어가는 길
 *
 * void fork_return_to_user(struct trap_frame *frame);   (rdi = frame)
 *   struct trap_frame 은 인터럽트/시스템 콜 진입 스텁이 쌓는 것과 같은 배치다 (include/interrupt.h):
 *   r15 ... rax, vector, error_code, rip, cs, rflags, rsp, ss
 *   범용 레지스터를 복원하고 vector/error_code 를 건너뛰면 iretq 가 원하는 프레임이 남는다.
 *   GS 는 커널의 평상시 상태 그대로다 (syscall_entry.s 의 [GS 규칙]: ring3 복귀는 swapgs 없이 iretq).
 */
    .section .text
    .global fork_return_to_user
    .type fork_return_to_user, @function
fork_return_to_user:
    cli                             /* 프레임을 꺼내는 동안 인터럽트가 끼어들지 않게 (IF 는 iretq 가 프레임에서 복원) */
    movq %rdi, %rsp
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
    .size fork_return_to_user, . - fork_return_to_user

    .section .note.GNU-stack,"",@progbits
