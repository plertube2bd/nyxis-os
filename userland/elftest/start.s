/*
 * start.s - elftest 의 진입점 (crt0 역할의 최소 구현: 커널 테스트용 fixture)
 *
 * 커널이 넘겨주는 System V AMD64 초기 스택: [rsp]=argc, [rsp+8]=argv[0]..., NULL, envp..., NULL, auxv...
 * 진입 시 rsp 는 16바이트 정렬이다. call 직전에 정렬을 다시 보장하고 main 의 반환값으로
 * NxProcessExit(65) 을 호출한다.
 *
 * 회귀 검사 두 가지를 여기서 한다 (C 코드가 레지스터를 건드리기 전이어야 하므로):
 *  - 진입 시 범용 레지스터(rsp 제외)가 전부 0 이어야 한다. 커널 레지스터 값이 새면 종료 코드 90.
 *  - 종료는 `syscall` 명령으로 한다. 예전 커널은 syscall 경로에서 프로세스가 끝나면 GS 가 뒤바뀐 채
 *    남아, 그 뒤 아무 프로세스의 `syscall` 이 커널 패닉을 일으켰다 (syscall_entry.s 의 [GS 규칙]).
 */
    .section .text
    .global _start
    .type _start, @function
_start:
    orq %rbx, %rax
    orq %rcx, %rax
    orq %rdx, %rax
    orq %rsi, %rax
    orq %rdi, %rax
    orq %rbp, %rax
    orq %r8, %rax
    orq %r9, %rax
    orq %r10, %rax
    orq %r11, %rax
    orq %r12, %rax
    orq %r13, %rax
    orq %r14, %rax
    orq %r15, %rax
    jnz leaked_regs

    xorl %ebp, %ebp
    movq (%rsp), %rdi           /* argc */
    leaq 8(%rsp), %rsi          /* argv */
    andq $-16, %rsp
    call elftest_main
    movl %eax, %edi             /* exit code */
exit:
    movl $65, %eax              /* NxProcessExit */
    syscall
1:  hlt                         /* 도달하면 안 된다. 유저 모드의 hlt 는 #GP 로 프로세스가 종료된다 */
    jmp 1b

leaked_regs:
    movl $90, %edi
    jmp exit
    .size _start, . - _start

    .section .note.GNU-stack,"",@progbits
