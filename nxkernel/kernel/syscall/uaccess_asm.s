/*
 * uaccess_asm.s - 사용자 메모리 접근용 복사 루틴 (예외 복구 지원)
 *
 * long nx_uaccess_copy(void *dst, const void *src, unsigned long len);
 *   반환: 0 = 성공, -1 = 페이지 폴트(접근 불가 메모리)
 *
 * 시스템 콜은 사용자 포인터를 paging_is_user_range() 로 먼저 검사하지만, 검사와 실제 접근 사이에
 * 다른 실행 흐름이 매핑을 바꿀 수 있다 (검사-사용 경쟁, TOCTOU). 그래서 실제 복사는 이 루틴 하나로만 하고,
 * 여기서 발생한 #PF 는 커널 패닉이 아니라 "-1 반환" 으로 복구한다 (uaccess_fixup, uaccess.c 참고).
 *
 * 폴트가 나는 명령은 nx_uaccess_insn (rep movsb) 하나뿐이다. 폴트 시 CPU 는 rip 를 그 명령에 그대로 두므로
 * 핸들러가 rip 를 nx_uaccess_fault 로 바꿔 돌려보내면 이 함수가 -1 을 반환한다.
 */
    .section .text

    .global nx_uaccess_copy
    .type nx_uaccess_copy, @function
nx_uaccess_copy:
    cld
    movq %rdx, %rcx
    .global nx_uaccess_insn
nx_uaccess_insn:
    rep movsb
    xorl %eax, %eax
    ret

    .global nx_uaccess_fault
nx_uaccess_fault:
    movq $-1, %rax
    ret
    .size nx_uaccess_copy, . - nx_uaccess_copy

    .section .note.GNU-stack,"",@progbits
