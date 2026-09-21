/*
 * selftest.c - 커널 부팅 후 실행되는 자체 점검 (make SELFTEST=1 로 빌드할 때만 포함)
 *
 * NYX_SELFTEST 값:
 *   1 : 기능 점검 (시스템 콜, printk 서식, 커널 스레드 전환, 타이머, 페이징 API)
 *   2 : #DE (0 으로 나누기) 발생 -> 예외 덤프 + 패닉이 나와야 함
 *   3 : #PF (NULL 역참조)   발생 -> 예외 덤프 + 패닉이 나와야 함 (0 번 페이지 매핑 해제 확인)
 *   4 : 커널 스택 오버플로  -> 가드 페이지 #PF -> IST 스택 #DF 덤프 후 패닉이 나와야 함
 *   5 : .text 에 쓰기       -> W^X(CR0.WP) 로 #PF 가 나와야 함
 *   6 : 데이터 영역 실행    -> NX 로 #PF(instruction fetch) 가 나와야 함
 *   7 : 잘못된 명령(ud2)    -> #UD
 */

#include "nyxis.h"
#include "console/outputs/printk.h"
#include "kernel/paging/paging.h"
#include "kernel/process/process.h"
#include "kernel/syscall/syscall.h"
#include "kernel/timer/pit/pit_base.h"
#include "kernel/error_handling/panic.h"
#include "memory.h"
#include "string.h"
#include "phys.h"
#include "kernel/kernel.h"

#ifndef NYX_SELFTEST
#define NYX_SELFTEST 1
#endif

void nyx_selftest(void);

static int g_failures = 0;

static void check(int cond, const char *what)
{
    if (cond) {
        printk("  [ OK ] %s\n", what);
    } else {
        printk("  [FAIL] %s\n", what);
        g_failures++;
    }
}

/* int 0x80 을 실제로 호출한다 (유저 래퍼와 같은 레지스터 규약) */
static i64 sys(u64 nr, u64 a0)
{
    i64 ret;

    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(nr), "D"(a0) : "memory");
    return ret;
}

static volatile int g_thread_a = 0;
static volatile int g_thread_b = 0;

static void thread_a(void *arg)
{
    int i;

    (void)arg;
    for (i = 0; i < 5; i++) {
        g_thread_a++;
        schedule();
    }
}

static void thread_b(void *arg)
{
    int i;

    (void)arg;
    for (i = 0; i < 5; i++) {
        g_thread_b += 2;
        schedule();
    }
}

static u8 g_user_page[4096] __attribute__((aligned(4096)));

/* ring 3 테스트용 사용자 페이지 (주소 배치는 selftest_user.s 주석 참고) */
static u8 g_user_code[4096] __attribute__((aligned(4096)));
static u8 g_user_data[4096] __attribute__((aligned(4096)));
static u8 g_user_stack[4096] __attribute__((aligned(4096)));

extern u8 user_prog1_start[];
extern u8 user_prog1_end[];
extern u8 user_prog2_start[];
extern u8 user_prog2_end[];

#define USER_CODE_VADDR   0x400000UL
#define USER_DATA_VADDR   0x401000UL
#define USER_STACK_VADDR  0x402000UL
#define USER_SENTINEL     0xDEADBEEFDEADBEEFUL

/* 커널 스레드가 ring 3 로 내려간다 (프로그램이 NxProcessExit 하거나 예외로 종료될 때까지) */
static void user_runner(void *arg)
{
    (void)arg;
    enter_ring3((void *)USER_CODE_VADDR, (void *)(USER_STACK_VADDR + PAGE_SIZE));
}

/* 살아 있는(종료되지 않은) 프로세스 수 */
static int live_processes(void)
{
    process_t *p;
    int n = 0;

    for (p = process_list; p; p = p->next) {
        if (p->in_use && p->state != PROCESS_TERMINATED)
            n++;
    }
    return n;
}

static void run_user_program(const u8 *start, const u8 *end)
{
    u32 i;

    memset(g_user_code, 0xCC, sizeof(g_user_code));
    memcpy(g_user_code, start, (usize)(end - start));

    for (i = 0; i < 8; i++)
        ((volatile u64 *)g_user_data)[i] = USER_SENTINEL;

    if (process_create(user_runner, nNULL) != Nok) {
        check(0, "process_create(user_runner)");
        return;
    }

    /* 사용자 프로그램이 끝날 때까지 양보하며 대기 (최대 약 1초) */
    for (i = 0; i < 200 && live_processes() > 1; i++) {
        schedule();
        sleep_ms(5);
    }
}

static void ring3_tests(void)
{
    volatile u64 *r = (volatile u64 *)g_user_data;

    printk("SELFTEST: ring 3 (user mode)\n");

    check(paging_map_page((void *)(usize)virt_to_phys(g_user_code), (void *)USER_CODE_VADDR, PAGE_USER) == Nok,
          "map user code page (R-X)");
    check(paging_map_page((void *)(usize)virt_to_phys(g_user_data), (void *)USER_DATA_VADDR,
                          PAGE_RW | PAGE_USER | PAGE_NX) == Nok, "map user data page (RW-)");
    check(paging_map_page((void *)(usize)virt_to_phys(g_user_stack), (void *)USER_STACK_VADDR,
                          PAGE_RW | PAGE_USER | PAGE_NX) == Nok, "map user stack page (RW-)");

    run_user_program(user_prog1_start, user_prog1_end);
    check(live_processes() == 1, "user program exited via NxProcessExit (syscall)");
    check(r[0] == 0, "ring3: syscall instruction -> NxDebugNop == 0");
    check(r[1] == 0, "ring3: int 0x80 -> NxDebugNop == 0");
    check(r[2] == 0, "ring3: NxKernelPrint(user string) == 0");
    check((i64)r[3] == (i64)NinvalidPointer, "ring3: NxKernelPrint(NULL) -> NinvalidPointer");
    check((i64)r[4] == (i64)NinvalidPointer, "ring3: NxKernelPrint(kernel address) -> NinvalidPointer");
    check(r[5] == 0, "ring3: NxYield == 0");

    run_user_program(user_prog2_start, user_prog2_end);
    check(live_processes() == 1, "user-mode #GP (hlt) terminated only the user process; kernel survived");
}

#if NYX_SELFTEST == 4
static unsigned long recurse(unsigned long n)
{
    volatile char pad[512];

    pad[0] = (char)n;
    if (n == 0xFFFFFFFFFFFFFFFFUL)      /* 컴파일러의 무한 재귀 경고/제거를 피하기 위한 (도달 불가) 종료 조건 */
        return 0;
    return recurse(n + 1) + (unsigned long)pad[0];
}
#endif

static void functional_tests(void)
{
    u32 i;
    u64 t0;
    u64 t1;
    void *vaddr = (void *)0x180000000UL;   /* 6GiB: 항등 매핑 범위 밖 (유저 영역) */

    printk("SELFTEST: functional tests\n");

    /* --- printk 서식 --- */
    printk("  fmt: [%08x] [%lu] [%s] [%c] [%5d] [%-] [%X]\n",
           0xBEEFU, 18446744073709551615UL, "str", 'Z', -42, 0xABCU);
    printk("  fmt: [%p] [%016lx] [%r]\n", (void *)0x1234UL, 0xDEADBEEFCAFEUL, NinvalidArg);

    /* --- 시스템 콜 --- */
    check(sys(NX_SYS_DEBUG_NOP, 0) == 0, "syscall 777 (NxDebugNop) == 0");
    check(sys(99999, 0) == (i64)NsyscallFailed, "unknown syscall -> NsyscallFailed (negative)");
    check(sys(NX_SYS_KERNEL_PRINT, 0) == (i64)NinvalidPointer, "syscall 771 NULL pointer rejected");
    check(sys(NX_SYS_KERNEL_PRINT, (u64)(usize)"kernel string") == (i64)NinvalidPointer,
          "syscall 771 kernel-space pointer rejected (not user-accessible)");
    check(sys(NX_SYS_KERNEL_PRINT, 0xFFFFFFFFFFFFFFF0UL) == (i64)NinvalidPointer,
          "syscall 771 non-canonical/kernel-range pointer rejected");

    /* --- 페이징 API + 유저 포인터 검증 --- */
    memset(g_user_page, 0, sizeof(g_user_page));
    strcpy((char *)g_user_page, "hello from a user-mapped page via %s %x\n");

    check(paging_map_page((void *)(usize)virt_to_phys(g_user_page), vaddr, PAGE_RW | PAGE_USER | PAGE_NX) == Nok,
          "paging_map_page(RW|USER|NX)");
    check(paging_map_page((void *)(usize)virt_to_phys(g_user_page), vaddr, PAGE_USER) == NalreadyExists,
          "paging_map_page duplicate -> NalreadyExists");
    check(paging_map_page((void *)(usize)virt_to_phys(g_user_page), (void *)0x181000000UL, PAGE_RW | PAGE_USER) == Npermission,
          "paging_map_page RW+X rejected (W^X)");
    check(paging_is_user_range(vaddr, 4096, true), "paging_is_user_range(mapped, write)");
    check(!paging_is_user_range(vaddr, 4097, false), "paging_is_user_range spanning unmapped page rejected");
    check(!paging_is_user_range((void *)0x1000UL, 16, false), "paging_is_user_range(kernel/identity) rejected");
    check(sys(NX_SYS_KERNEL_PRINT, (u64)(usize)vaddr) == 0, "syscall 771 with user page (format string not expanded above)");
    check(paging_unmap_page(vaddr) == Nok, "paging_unmap_page");
    check(!paging_is_user_range(vaddr, 1, false), "unmapped page no longer user-accessible");
    check(sys(NX_SYS_KERNEL_PRINT, (u64)(usize)vaddr) == (i64)NinvalidPointer, "syscall 771 after unmap rejected");

    /* --- 커널 스레드 컨텍스트 스위치 --- */
    check(process_create(thread_a, nNULL) == Nok, "process_create(thread_a)");
    check(process_create(thread_b, nNULL) == Nok, "process_create(thread_b)");
    for (i = 0; i < 40; i++)
        schedule();
    check(g_thread_a == 5 && g_thread_b == 10, "two kernel threads ran interleaved to completion");
    check(process_terminate(0) == Npermission, "cannot terminate idle/boot thread");

    /* --- 타이머 --- */
    t0 = timer_get_tick();
    sleep_ms(100);
    t1 = timer_get_tick();
    printk("  timer ticks during sleep_ms(100): %lu\n", (unsigned long)(t1 - t0));
    check(t1 - t0 >= 9 && t1 - t0 <= 12, "PIT tick ~100Hz (sleep_ms(100) -> 9..12 ticks)");

    /* --- 문자열/메모리 --- */
    {
        char buf[16];

        memset(buf, 'x', sizeof(buf));
        memmove(buf + 2, buf, 8);
        check(strlen("abc") == 3 && strcmp("abc", "abd") < 0 && strnlen("abcdef", 3) == 3, "string basics");
        check(strstr("haystack", "st") != nNULL && strstr("ab", "abc") == nNULL, "strstr end-of-haystack safe");
    }

    ring3_tests();

    if (g_failures == 0)
        printk("SELFTEST PASSED\n");
    else
        printk("SELFTEST FAILED (%d failures)\n", g_failures);
}

void nyx_selftest(void)
{
#if NYX_SELFTEST == 1
    functional_tests();
#elif NYX_SELFTEST == 2
    printk("SELFTEST: triggering #DE\n");
    /* C 의 1/x 는 컴파일러가 (x==1) 로 바꿔 버리므로 div 명령을 직접 실행한다 */
    __asm__ volatile ("xorl %%edx, %%edx\n\tmovl $1, %%eax\n\txorl %%ecx, %%ecx\n\tdivl %%ecx" : : : "eax", "ecx", "edx");
#elif NYX_SELFTEST == 3
    printk("SELFTEST: triggering #PF (NULL dereference)\n");
    printk("%d\n", *(volatile int *)0);
#elif NYX_SELFTEST == 4
    printk("SELFTEST: triggering kernel stack overflow\n");
    printk("%lu\n", recurse(0));
#elif NYX_SELFTEST == 5
    printk("SELFTEST: writing to .text\n");
    *(volatile u8 *)(usize)nyx_selftest = 0x90;
#elif NYX_SELFTEST == 6
    printk("SELFTEST: executing data page\n");
    g_user_page[0] = 0xC3;              /* ret */
    __asm__ volatile ("call *%0" : : "r"(g_user_page) : "memory");
#elif NYX_SELFTEST == 7
    printk("SELFTEST: triggering #UD\n");
    __asm__ volatile ("ud2");
#endif
    (void)g_user_page;
    (void)functional_tests;
    (void)kernel_panic_simple;
}
