/*
 * selftest_user.s - 자체 점검용 사용자 모드(ring 3) 프로그램 (selftest.c 가 사용자 페이지로 복사해 실행)
 *
 * 사용자 주소 배치 (selftest.c 와 일치):
 *   0x400000 : 코드 (R-X)   0x401000 : 결과 저장용 데이터 (RW-)   0x402000~0x403000 : 스택
 * 위치 독립 코드이며 문자열은 rip 상대 주소로 참조한다.
 */
    .section .rodata

    .global user_prog1_start
    .global user_prog1_end
user_prog1_start:
    movabsq $0x401000, %rbx         /* 결과 배열 */

    movl $777, %eax                 /* NxDebugNop via syscall 명령 -> 0 */
    syscall
    movq %rax, 0(%rbx)

    movl $777, %eax                 /* NxDebugNop via int 0x80 -> 0 */
    int $0x80
    movq %rax, 8(%rbx)

    movl $771, %eax                 /* NxKernelPrint(유효한 사용자 문자열) -> 0 */
    leaq umsg(%rip), %rdi
    syscall
    movq %rax, 16(%rbx)

    movl $771, %eax                 /* NxKernelPrint(NULL) -> NinvalidPointer (음수) */
    xorl %edi, %edi
    syscall
    movq %rax, 24(%rbx)

    movl $771, %eax                 /* NxKernelPrint(커널 주소) -> 거부되어야 함 */
    movabsq $0xFFFFFFFF80100000, %rdi
    int $0x80
    movq %rax, 32(%rbx)

    movl $5, %eax                   /* NxYield */
    syscall
    movq %rax, 40(%rbx)

    movl $65, %eax                  /* NxProcessExit */
    xorl %edi, %edi
    syscall
1:  jmp 1b
umsg:
    .asciz "hello from ring 3 (syscall + int 0x80)\n"
user_prog1_end:

    /* 특권 명령(hlt)을 실행 -> #GP -> 이 프로세스만 종료되고 커널은 계속 동작해야 한다 */
    .global user_prog2_start
    .global user_prog2_end
user_prog2_start:
    hlt
1:  jmp 1b
user_prog2_end:

    .section .note.GNU-stack,"",@progbits
