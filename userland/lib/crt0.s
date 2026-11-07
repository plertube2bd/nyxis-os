/*
 * crt0.s - Nyxis 유저 프로그램 진입점
 *
 * 커널이 넘겨주는 System V AMD64 초기 스택: [rsp]=argc, [rsp+8]=argv[0..argc-1], NULL, envp..., NULL, auxv
 * main(argc, argv, envp) 를 부르고 그 반환값으로 NxProcessExit(65) 한다.
 */
    .section .text
    .global _start
    .type _start, @function
_start:
    xorl %ebp, %ebp
    movq (%rsp), %rdi           /* argc */
    leaq 8(%rsp), %rsi          /* argv */
    leaq 8(%rsi,%rdi,8), %rdx   /* envp = argv + argc + 1 */
    andq $-16, %rsp
    call main
    movl %eax, %edi
    movl $65, %eax              /* NxProcessExit */
    syscall
1:  jmp 1b
    .size _start, . - _start

    .section .note.GNU-stack,"",@progbits
