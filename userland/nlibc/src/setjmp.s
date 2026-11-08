/*
 * setjmp.s - setjmp/longjmp (x86_64 System V). jmp_buf 배치는 library/setjmp.h:
 *   [0] rbx  [1] rbp  [2] r12  [3] r13  [4] r14  [5] r15  [6] rsp(복귀 후)  [7] 복귀 주소
 */
    .section .text

    .global setjmp
    .type setjmp, @function
setjmp:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rdx          /* setjmp 가 돌아간 뒤의 rsp */
    movq %rdx, 48(%rdi)
    movq (%rsp), %rdx           /* 복귀 주소 */
    movq %rdx, 56(%rdi)
    xorl %eax, %eax
    ret
    .size setjmp, . - setjmp

    .global longjmp
    .type longjmp, @function
longjmp:
    movl %esi, %eax
    testl %eax, %eax            /* C89 7.6.2.1: val 이 0 이면 setjmp 는 1 을 돌려준다 */
    jnz 1f
    incl %eax
1:  movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    jmpq *56(%rdi)
    .size longjmp, . - longjmp

    .section .note.GNU-stack,"",@progbits
