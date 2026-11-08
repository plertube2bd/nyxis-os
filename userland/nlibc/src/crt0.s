/*
 * crt0.s - nlibc 프로그램 진입점
 *
 * 커널이 넘겨주는 System V AMD64 초기 스택: [rsp]=argc, [rsp+8]=argv[0..argc-1], NULL, envp..., NULL, auxv
 * __nl_start(argc, argv, envp) 가 libc 를 준비하고 exit(main(...)) 한다 (돌아오지 않는다).
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
    call __nl_start
1:  hlt                         /* 도달하지 않는다 (ring3 에서 hlt 는 폴트 -> 커널이 종료시킨다) */
    jmp 1b
    .size _start, . - _start

    .section .note.GNU-stack,"",@progbits
