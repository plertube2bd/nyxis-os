/*
 * uaccess.c - 사용자 메모리 안전 접근 구현 (설계는 uaccess.h 참고)
 */

#include "kernel/syscall/uaccess.h"
#include "kernel/paging/paging.h"

/* uaccess.s */
extern long nx_uaccess_copy(void *dst, const void *src, unsigned long len);
extern u8 nx_uaccess_insn[];
extern u8 nx_uaccess_fault[];

Nstatus copy_from_user(void *dst, u64 user_src, usize len)
{
    if (len == 0)
        return NSTATUS_OK;
    if (!dst)
        return NinvalidArg;

    if (!paging_is_user_range((const void *)(usize)user_src, len, false))
        return NinvalidPointer;

    if (nx_uaccess_copy(dst, (const void *)(usize)user_src, (unsigned long)len) != 0)
        return NinvalidPointer;

    return NSTATUS_OK;
}

Nstatus copy_to_user(u64 user_dst, const void *src, usize len)
{
    if (len == 0)
        return NSTATUS_OK;
    if (!src)
        return NinvalidArg;

    if (!paging_is_user_range((const void *)(usize)user_dst, len, true))
        return NinvalidPointer;

    if (nx_uaccess_copy((void *)(usize)user_dst, src, (unsigned long)len) != 0)
        return NinvalidPointer;

    return NSTATUS_OK;
}

Nstatus copy_string_from_user(char *dst, u64 user_src, usize max)
{
    usize i;

    if (!dst || max == 0)
        return NinvalidArg;
    if (user_src == 0)
        return NinvalidPointer;

    for (i = 0; i < max; i++) {
        u64 addr = user_src + (u64)i;
        Nstatus status;

        if (addr < user_src)                 /* 주소 오버플로 */
            return NinvalidPointer;

        status = copy_from_user(&dst[i], addr, 1);
        if (NSTATUS_IS_ERR(status))
            return status;

        if (dst[i] == '\0')
            return NSTATUS_OK;
    }

    /* 종료 문자를 못 찾음: 너무 긴 문자열 */
    dst[max - 1] = '\0';
    return NpathTooLong;
}

bool uaccess_fixup(struct trap_frame *frame)
{
    /* 커널 모드의 폴트이고, 폴트 명령이 uaccess 복사 명령이며, 폴트 주소가 사용자 영역일 때만 복구한다.
     * (커널 포인터 쪽에서 난 폴트는 진짜 커널 버그이므로 복구하지 않고 패닉시킨다) */
    if ((frame->cs & 3UL) != 0UL)
        return false;
    if (frame->rip != (u64)(usize)nx_uaccess_insn)
        return false;
    if (read_cr2() >= USER_SPACE_END)
        return false;

    frame->rip = (u64)(usize)nx_uaccess_fault;
    return true;
}
