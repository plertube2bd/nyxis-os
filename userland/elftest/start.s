/*
 * start.s - elftest 의 진입점 (crt0 역할의 최소 구현: 커널 테스트용 fixture)
 *
 * 커널이 넘겨주는 System V AMD64 초기 스택: [rsp]=argc, [rsp+8]=argv[0]..., NULL, envp..., NULL, auxv...
 * 진입 시 rsp 는 16바이트 정렬이다. call 직전에 정렬을 다시 보장하고 main 의 반환값으로
 * NxProcessExit(65) 을 호출한다 (int 0x80 경로).
 */
    .section .text
    .global _start
    .type _start, @function
_start:
    xorl %ebp, %ebp
    movq (%rsp), %rdi           /* argc */
    leaq 8(%rsp), %rsi          /* argv */
    andq $-16, %rsp
    call elftest_main
    movl %eax, %edi             /* exit code */
    movl $65, %eax              /* NxProcessExit */
    int $0x80
1:  hlt                         /* 도달하면 안 된다. 유저 모드의 hlt 는 #GP 로 프로세스가 종료된다 */
    jmp 1b
    .size _start, . - _start

    .section .note.GNU-stack,"",@progbits
