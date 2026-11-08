/*
 * sigtramp.s - 신호 처리기가 돌아갈 곳 (restorer)
 *
 * 커널은 처리기를 부를 때 반환 주소로 이 코드를 넣는다. 처리기의 ret 가 여기로 오면 rsp 는 커널이 쌓은
 * 신호 문맥을 가리키고, 신호 복귀 시스템 콜이 그 문맥(처리기 직전의 레지스터)으로 돌아간다. 돌아오지 않는다.
 *   __nl_sigtramp_nyx   : Nyxis NxSignalReturn (79, include/nyx_abi.h)
 *   __nl_sigtramp_linux : Linux rt_sigreturn (15) - 호스트 검사용 (tests/host/nlibc/sys_linux.c)
 */
    .section .text

    .global __nl_sigtramp_nyx
    .type __nl_sigtramp_nyx, @function
__nl_sigtramp_nyx:
    movl $79, %eax
    syscall
    ud2
    .size __nl_sigtramp_nyx, . - __nl_sigtramp_nyx

    .global __nl_sigtramp_linux
    .type __nl_sigtramp_linux, @function
__nl_sigtramp_linux:
    movl $15, %eax
    syscall
    ud2
    .size __nl_sigtramp_linux, . - __nl_sigtramp_linux

    .section .note.GNU-stack,"",@progbits
