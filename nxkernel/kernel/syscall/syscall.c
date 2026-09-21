/*
 * syscall.c - int 0x80 시스템 콜 처리
 *
 * [기존 코드의 문제 (모두 수정)]
 *  1. 유저가 넘긴 포인터를 검증 없이 printk(message) 로 사용
 *       -> (a) 커널 메모리를 임의로 읽는 원시 능력(arbitrary kernel read)
 *          (b) 문자열이 서식 문자열로 해석되어 %x/%s 로 커널 스택 유출 (format string 공격)
 *          (c) 종료되지 않은 문자열이면 무한 읽기
 *     -> 이제 paging_is_user_range 로 유저 접근 가능 범위인지 검사하고, 최대
 *        SYSCALL_STRING_MAX 바이트만 커널 버퍼로 복사한 뒤 printk("%s", buf) 로 출력한다.
 *  2. naked asm 스텁의 인자 레지스터 재배치가 SysV 호출 규약과 맞지 않았다.
 *       (syscall_dispatch 의 4번째 인자는 rcx 인데 r10 에 넣음 등) 또한 호출 시 스택
 *       정렬이 보장되지 않았고, 예외/IRQ 와 다른 별도 진입 경로를 유지해야 했다.
 *     -> 이제 다른 모든 벡터와 같은 스텁(isr_stubs.s)을 쓰고, C 에서 trap_frame 으로
 *        인자를 꺼낸다.
 *  3. 반환값이 문서(syscalls.txt)의 "rax < 0 이면 오류" 와 달랐다.
 *     -> Nstatus(음수=오류)를 64비트로 부호 확장하여 rax 로 돌려준다.
 */

#include "kernel/syscall/syscall.h"
#include "kernel/paging/paging.h"
#include "console/outputs/printk.h"
#include "memory.h"
#include "lowlevel.h"
#include "phys.h"
#include "kernel/process/process.h"

/*
 * 유저 문자열을 커널 버퍼로 안전하게 복사한다.
 * dst 는 max 바이트 이상이어야 하며 성공 시 항상 NUL 로 끝난다.
 */
static Nstatus copy_string_from_user(char *dst, u64 user_ptr, usize max)
{
    usize i;

    if (!dst || max == 0)
        return NinvalidArg;
    if (user_ptr == 0)
        return NinvalidPointer;

    for (i = 0; i < max; i++) {
        u64 addr = user_ptr + (u64)i;

        /* 오버플로 또는 유저 영역 이탈 검사 후, 바이트 단위로 접근 가능성 확인 */
        if (addr < user_ptr)
            return NinvalidPointer;
        if (!paging_is_user_range((const void *)(usize)addr, 1, false))
            return NinvalidPointer;

        dst[i] = *(const volatile char *)(usize)addr;
        if (dst[i] == '\0')
            return NSTATUS_OK;
    }

    /* 종료 문자를 못 찾음: 너무 긴 문자열 */
    dst[max - 1] = '\0';
    return NpathTooLong;
}

i64 syscall_dispatch(
    u64 syscall_nr,
    u64 a0,
    u64 a1,
    u64 a2,
    u64 a3,
    u64 a4,
    u64 a5
) {
    /* 현재 정의된 시스템 콜은 인자를 최대 1개만 쓴다 */
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    switch (syscall_nr) {
    case NX_SYS_KERNEL_PRINT: {
        char buffer[SYSCALL_STRING_MAX];
        Nstatus status = copy_string_from_user(buffer, a0, sizeof(buffer));

        if (NSTATUS_IS_ERR(status))
            return (i64)status;

        /* 반드시 "%s" 로 출력: 유저 문자열을 서식 문자열로 쓰지 않는다 */
        printk("%s", buffer);
        return 0;
    }

    case NX_SYS_DEBUG_NOP:
        return 0;

    case NX_SYS_YIELD:
        schedule();
        return 0;

    case NX_SYS_PROCESS_EXIT:
        process_exit();
        /* 반환하지 않는다 */

    default:
        return (i64)NsyscallFailed;
    }
}

void syscall_handle(struct trap_frame *frame)
{
    i64 result = syscall_dispatch(
        frame->rax,
        frame->rdi,
        frame->rsi,
        frame->rdx,
        frame->r10,
        frame->r8,
        frame->r9
    );

    /* iretq 후 호출자의 rax 로 전달된다 */
    frame->rax = (u64)result;
}

/* ------------------------------------------------------------------ */
/* syscall / sysret                                                    */
/* ------------------------------------------------------------------ */

#define MSR_EFER         0xC0000080U
#define MSR_STAR         0xC0000081U
#define MSR_LSTAR        0xC0000082U
#define MSR_SFMASK       0xC0000084U
#define MSR_GS_BASE      0xC0000101U
#define MSR_KERNEL_GS    0xC0000102U
#define EFER_SCE         1UL

/* syscall 진입 시 RFLAGS 에서 지울 비트: IF(인터럽트), TF(단일 스텝), DF(방향), AC, NT */
#define SFMASK_VALUE     (0x200UL | 0x100UL | 0x400UL | 0x40000UL | 0x4000UL)

struct cpu_local g_cpu_local;

Nstatus syscall_init(void)
{
    u32 a, b, c, d;

    cpuid(0x80000000U, &a, &b, &c, &d);
    if (a < 0x80000001U)
        return NdeviceMissing;
    cpuid(0x80000001U, &a, &b, &c, &d);
    if (!(d & (1U << 11)))               /* SYSCALL/SYSRET 지원 여부 */
        return NdeviceMissing;

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    wrmsr(MSR_STAR, ((u64)STAR_USER_BASE << 48) | ((u64)STAR_KERNEL_BASE << 32));
    wrmsr(MSR_LSTAR, (u64)(usize)syscall_entry);
    wrmsr(MSR_SFMASK, SFMASK_VALUE);

    /*
     * 커널은 GS 를 쓰지 않으므로 평상시 GS.base = 0 (사용자 GS 도 KERNEL_GS_BASE 에 보관하지 않음).
     * syscall_entry.s 가 swapgs 로 GS.base 를 g_cpu_local 로 바꿨다가 sysret 직전에 되돌린다.
     */
    wrmsr(MSR_GS_BASE, 0);
    wrmsr(MSR_KERNEL_GS, (u64)(usize)&g_cpu_local);

    return NSTATUS_OK;
}

void syscall_bad_return(void)
{
    printk("syscall: invalid return address, terminating process\n");
    process_exit();
}
