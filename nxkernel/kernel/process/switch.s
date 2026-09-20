/*
 * switch.s - 커널 스레드 컨텍스트 스위치
 *
 * void cpu_switch_context(u64 *save_rsp, u64 new_rsp);
 *   rdi = 현재 스레드의 rsp 를 저장할 위치
 *   rsi = 전환할 스레드의 rsp
 *
 * SysV ABI 에서 함수 호출을 넘어 보존되어야 하는 레지스터(callee-saved)
 * rbx, rbp, r12~r15 만 저장/복원하면 충분하다. (호출자 저장 레지스터는 컴파일러가
 * 이미 함수 호출 전에 알아서 저장한다)
 *
 * 새 스레드의 초기 스택은 process.c 가 다음 순서로 만들어 둔다 (낮은 주소 -> 높은):
 *   r15, r14, r13(=스레드 인자), r12(=진입 함수), rbx, rbp, 반환주소(process_entry_trampoline)
 */
    .section .text

    .global cpu_switch_context
    .type cpu_switch_context, @function
cpu_switch_context:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)       /* 현재 rsp 저장 */
    movq %rsi, %rsp         /* 새 스레드의 스택으로 전환 */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
    .size cpu_switch_context, . - cpu_switch_context

    /*
     * 새 스레드가 처음 실행되는 지점. cpu_switch_context 의 ret 로 여기에 도착한다.
     * 이 시점에 rsp 는 16바이트 정렬이므로 call 을 바로 할 수 있다.
     */
    .global process_entry_trampoline
    .type process_entry_trampoline, @function
process_entry_trampoline:
    sti                     /* 스케줄러는 인터럽트를 끈 채 전환하므로 새 스레드는 여기서 켠다 */
    movq %r13, %rdi         /* 스레드 함수 인자 */
    call *%r12              /* 스레드 함수 호출 */
    call process_thread_exit
1:  hlt                     /* 도달하면 안 됨 */
    jmp 1b
    .size process_entry_trampoline, . - process_entry_trampoline

    .section .note.GNU-stack,"",@progbits
