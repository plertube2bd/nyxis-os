/*
 * isr_stubs.s - 인터럽트/예외 진입 스텁 (벡터 0~255)
 *
 * 각 벡터마다 작은 스텁이 있어서 "벡터 번호" 와 (CPU 가 에러코드를 push 하지 않는
 * 벡터의 경우) "가짜 에러코드 0" 을 스택에 push 한 뒤 공통 루틴 isr_common 으로 점프한다.
 * isr_common 은 범용 레지스터를 저장하고 C 함수 interrupt_dispatch(struct trap_frame *)
 * 를 호출한 다음 복원하고 iretq 로 복귀한다.
 *
 * CPU 가 에러코드를 push 하는 벡터: 8, 10, 11, 12, 13, 14, 17, 21, 29, 30
 *
 * 스택 정렬: 64비트 모드에서 CPU 는 인터럽트 전달 시 rsp 를 16바이트로 정렬한 뒤
 * 5개 값(ss, rsp, rflags, cs, rip)을 push 한다. 이후 에러코드(8) + 벡터(8) + 레지스터
 * 15개(120) 를 더 push 하므로 (40 + 16 + 120 = 176 = 16 * 11) call 직전 rsp 는
 * 16바이트 정렬이다. (SysV ABI 요구 충족)
 *
 * 참고: 이 커널은 sysret/swapgs 를 쓰지 않으므로 swapgs 는 필요 없다.
 */

    .altmacro

    .section .text

    .macro isr_entry n
    .align 16
    .global isr_stub_\n
isr_stub_\n:
    .if ((\n) == 8) || ((\n) == 10) || ((\n) == 11) || ((\n) == 12) || ((\n) == 13) || ((\n) == 14) || ((\n) == 17) || ((\n) == 21) || ((\n) == 29) || ((\n) == 30)
    /* CPU 가 에러코드를 이미 push 함 */
    .else
    pushq $0            /* 가짜 에러코드 */
    .endif
    pushq $\n           /* 벡터 번호 */
    jmp isr_common
    .endm

    .set i, 0
    .rept 256
    isr_entry %i
    .set i, i + 1
    .endr

    .align 16
isr_common:
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

    cld                     /* SysV ABI: 방향 플래그는 함수 진입 시 0 */
    movq %rsp, %rdi         /* 인자: struct trap_frame * */
    call interrupt_dispatch

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

    addq $16, %rsp          /* 벡터 + 에러코드 제거 */
    iretq

    /* 스텁 주소 테이블 (interrupt.c 에서 IDT 를 채울 때 사용) */
    .macro isr_ptr n
    .quad isr_stub_\n
    .endm

    .section .rodata
    .align 8
    .global isr_stub_table
isr_stub_table:
    .set i, 0
    .rept 256
    isr_ptr %i
    .set i, i + 1
    .endr

    .section .note.GNU-stack,"",@progbits
