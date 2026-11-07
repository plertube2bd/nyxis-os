/*
 * selftest.c - 커널 부팅 후 실행되는 자체 점검 (make SELFTEST=1 로 빌드할 때만 포함)
 *
 * NYX_SELFTEST 값:
 *   1 : 기능 점검 (시스템 콜, printk 서식, 커널 스레드 전환, 타이머, 페이징 API,
 *       ring3 flat 바이너리, NxProcessCreate/ELF 로더/힙/프로세스별 주소 공간)
 *   2 : #DE (0 으로 나누기) 발생 -> 예외 덤프 + 패닉이 나와야 함
 *   3 : #PF (NULL 역참조)   발생 -> 예외 덤프 + 패닉이 나와야 함 (0 번 페이지 매핑 해제 확인)
 *   4 : 커널 스택 오버플로  -> 가드 페이지 #PF -> IST 스택 #DF 덤프 후 패닉이 나와야 함
 *   5 : .text 에 쓰기       -> W^X(CR0.WP) 로 #PF 가 나와야 함
 *   6 : 데이터 영역 실행    -> NX 로 #PF(instruction fetch) 가 나와야 함
 *   7 : 잘못된 명령(ud2)    -> #UD
 *   8 : 커널 스레드 스택 오버플로 -> 그 스레드의 가드 페이지 #PF -> #DF 덤프 후 패닉 (옆 스택을 덮어쓰지 않음)
 *   9 : kfree 이중 해제     -> "kfree: double free" 패닉 (조용한 힙 손상 대신)
 */

#include "nyxis.h"
#include "console/outputs/printk.h"
#include "kernel/paging/paging.h"
#include "kernel/mm/pfa.h"
#include "kernel/process/process.h"
#include "kernel/process/spawn.h"
#include "kernel/process/waitq.h"
#include "kernel/mm/kheap.h"
#include "console/inputs/tty.h"
#include "kernel/syscall/syscall.h"
#include "kernel/timer/pit/pit_base.h"
#include "kernel/error_handling/panic.h"
#include "memory.h"
#include "string.h"
#include "phys.h"
#include "nyx_abi.h"

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

/* --- 일반 페이징 API 점검용 페이지 (idle 스레드가 공유하는 "커널 전용" 주소 공간에 매핑한다:
 * paging_kernel_addrspace() 의 문서 참고 — 실제 프로세스가 아니라 매핑 API 자체를 검사할
 * 때만 쓰는 방법이다) */
static u8 g_scratch_page[4096] __attribute__((aligned(4096)));

/* ring 3 테스트용 사용자 페이지: 반드시 PFA 로 할당한다. 이 프로세스들은 전용 주소 공간을
 * 갖고, 종료되면 process_reap() 이 paging_addrspace_destroy() 로 그 안의 프레임을 전부
 * pfa_free() 하기 때문이다 — 커널 .bss 정적 배열을 매핑하면 그 프레임이 실수로 "회수"되어
 * 시스템 전체가 오염된다. */
extern u8 user_prog1_start[];
extern u8 user_prog1_end[];
extern u8 user_prog2_start[];
extern u8 user_prog2_end[];

#define USER_CODE_VADDR   0x400000UL
#define USER_DATA_VADDR   0x401000UL
#define USER_STACK_VADDR  0x402000UL
#define USER_SENTINEL     0xDEADBEEFDEADBEEFUL

/* 사용 중인 프로세스 슬롯 수 (종료됐지만 참조가 남은 슬롯 포함) */
static int count_slots(void)
{
    process_t *p;
    int n = 0;

    for (p = process_list; p; p = p->next) {
        if (p->in_use)
            n++;
    }
    return n;
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

/*
 * 새 전용 주소 공간을 만들어 [start,end) 의 flat 바이너리를 USER_CODE_VADDR 에, 사용자
 * 스택을 USER_STACK_VADDR 에 매핑하고 ring3 로 실행한다. 종료를 기다려 종료 코드를 수거한 뒤,
 * 데이터 페이지의 커널 쪽 별칭(HHDM)과 물리 주소를 돌려준다.
 *
 * 데이터 페이지는 PAGE_NOFREE 로 매핑한다: 종료된 프로세스의 주소 공간은 schedule() 이 즉시
 * 회수하므로, 소유권을 주소 공간에 주면 결과를 읽기 전에 프레임이 해제된다. 호출자가 결과를 다
 * 읽은 뒤 *out_data_phys 를 pfa_free() 해야 한다. 실패하면 *out_data_kva 를 NULL 로 남긴다.
 */
static void run_user_program(const u8 *start, const u8 *end, volatile u64 **out_data_kva,
                             u64 *out_data_phys, i32 *out_exit_code)
{
    addr_space_t as;
    u64 code_phys;
    u64 data_phys;
    u64 stack_phys;
    u32 i;
    process_t *proc = nNULL;
    Nstatus status;

    *out_data_kva = nNULL;
    *out_data_phys = 0;
    *out_exit_code = 0x7FFFFFFF;

    if (NSTATUS_IS_ERR(paging_addrspace_create(&as))) {
        check(0, "paging_addrspace_create() for ring3 test");
        return;
    }
    if (NSTATUS_IS_ERR(pfa_alloc(&code_phys)) ||
        NSTATUS_IS_ERR(pfa_alloc(&data_phys)) ||
        NSTATUS_IS_ERR(pfa_alloc(&stack_phys))) {
        check(0, "pfa_alloc() for ring3 test pages");
        paging_addrspace_destroy(&as);
        return;
    }

    memset((void *)(usize)phys_to_virt(code_phys), 0xCC, PAGE_SIZE);
    memcpy((void *)(usize)phys_to_virt(code_phys), start, (usize)(end - start));
    for (i = 0; i < 8; i++)
        ((volatile u64 *)(usize)phys_to_virt(data_phys))[i] = USER_SENTINEL;

    check(paging_map_page(&as, (void *)(usize)code_phys, (void *)USER_CODE_VADDR, PAGE_USER) == Nok,
          "map user code page (R-X)");
    check(paging_map_page(&as, (void *)(usize)data_phys, (void *)USER_DATA_VADDR,
                          PAGE_RW | PAGE_USER | PAGE_NX | PAGE_NOFREE) == Nok, "map user data page (RW-, not owned)");
    check(paging_map_page(&as, (void *)(usize)stack_phys, (void *)USER_STACK_VADDR,
                          PAGE_RW | PAGE_USER | PAGE_NX) == Nok, "map user stack page (RW-)");

    status = process_create_user(as, USER_CODE_VADDR, USER_STACK_VADDR + PAGE_SIZE, 0, &proc);
    if (status != Nok) {
        check(0, "process_create_user() for ring3 test");
        paging_addrspace_destroy(&as);
        (void)pfa_free(data_phys);              /* NOFREE 라서 주소 공간 파기로는 회수되지 않는다 */
        return;
    }

    /* 사용자 프로그램이 끝날 때까지 기다려(최대 약 1초, idle 은 양보하며 폴링) 종료 코드를 받고 참조를 놓는다 */
    check(process_wait(proc, 100, out_exit_code) == Nok, "ring3 test program terminated (process_wait)");
    process_release(proc);

    *out_data_kva = (volatile u64 *)(usize)phys_to_virt(data_phys);
    *out_data_phys = data_phys;
}

static void ring3_tests(void)
{
    volatile u64 *r = nNULL;
    u64 data_phys = 0;
    i32 code = 0;

    printk("SELFTEST: ring 3 (user mode)\n");

    run_user_program(user_prog1_start, user_prog1_end, &r, &data_phys, &code);
    check(r != nNULL, "run_user_program(user_prog1) produced a data page");
    if (!r)
        return;

    check(live_processes() == 1, "user program exited via NxProcessExit (syscall)");
    check(code == 0, "user program's exit code (NxProcessExit(0) via syscall) was collected by its parent");
    check(r[0] == 0, "ring3: syscall instruction -> NxDebugNop == 0");
    check(r[1] == 0, "ring3: int 0x80 -> NxDebugNop == 0");
    check(r[2] == 0, "ring3: NxKernelPrint(user string) == 0");
    check((i64)r[3] == (i64)NinvalidPointer, "ring3: NxKernelPrint(NULL) -> NinvalidPointer");
    check((i64)r[4] == (i64)NinvalidPointer, "ring3: NxKernelPrint(kernel address) -> NinvalidPointer");
    check(r[5] == 0, "ring3: NxYield == 0");

    /* --- Core: 버전/시간/수면/시스템 정보 --- */
    check(r[6] == NX_ABI_VERSION, "ring3: NxGetVersion == NX_ABI_VERSION");
    check(r[8] == 0, "ring3: NxSleep(20ms) == 0");
    check(r[9] > r[7], "ring3: NxGetTime increased across NxSleep(20ms)");
    check(r[9] - r[7] >= 10000000UL, "ring3: elapsed >= ~10ms in nanoseconds (loose bound)");
    check(r[10] == 0, "ring3: NxSysInfo() == 0");
    {
        struct nx_sysinfo si;

        memcpy(&si, (const void *)(r + (0x140 / 8)), sizeof(si));
        check(si.abi_version_major == (u32)NX_ABI_MAJOR && si.abi_version_minor == (u32)NX_ABI_MINOR,
              "ring3: NxSysInfo abi version matches NX_ABI_VERSION");
        check(si.page_size == (u32)PAGE_SIZE, "ring3: NxSysInfo page_size == 4096");
        check(si.timer_hz > 0, "ring3: NxSysInfo timer_hz > 0");
        check(si.process_count >= 1U, "ring3: NxSysInfo process_count includes this process");
    }

    /* --- File I/O: open/read/stat/seek/close --- */
    check((i64)r[11] >= 0, "ring3: NxOpen(app:/hellowld.run, READ) succeeded");
    check(r[12] == 5, "ring3: NxRead first chunk == 5 bytes");
    check(memcmp((const void *)(r + (0x200 / 8)), "Hello", 5) == 0, "ring3: NxRead first chunk content == \"Hello\"");
    check(r[13] == 24, "ring3: NxRead second chunk == remaining 24 bytes (5+24 == 29-byte file)");
    check(memcmp((const u8 *)r + 0x205, " from app:/hellowld.run\n", 24) == 0,
          "ring3: NxRead second chunk content matches file tail");
    check(r[14] == 0, "ring3: NxStat() == 0");
    {
        struct nx_stat st;

        memcpy(&st, (const void *)(r + (0x100 / 8)), sizeof(st));
        check(st.size == 29, "ring3: NxStat size == 29 (file size)");
        check(st.type == NX_TYPE_FILE, "ring3: NxStat type == NX_TYPE_FILE");
        check((st.rights & NX_RIGHT_WRITE) == 0, "ring3: NxStat rights exclude WRITE (opened read-only)");
    }
    check(r[15] == 0, "ring3: NxSeek(SET, 0) -> offset 0");
    check(r[16] == 5, "ring3: NxRead after seek(0) == 5 bytes again");
    check(memcmp((const u8 *)r + 0x280, "Hello", 5) == 0, "ring3: NxRead after seek content == \"Hello\"");

    /* --- 핸들: 복제는 권한을 넘길 수 없고, 닫힌 핸들은 재사용할 수 없다 --- */
    check((i64)r[17] >= 0, "ring3: NxDuplicateHandle(READ-only file, mask=ALL) succeeded");
    check((i64)r[18] == (i64)Npermission,
          "ring3: writing through a duplicated read-only handle is rejected (no privilege escalation via dup)");
    check(r[19] == 0, "ring3: NxClose(original file handle) == 0");
    check(r[20] == 0, "ring3: NxClose(duplicated file handle) == 0");
    check((i64)r[21] == (i64)NinvalidArg, "ring3: closing an already-closed handle is rejected (no double-close)");

    /* --- 콘솔 쓰기 + stdout 복제 --- */
    check(r[22] == 19, "ring3: NxWrite(stdout, \"ring3 stdout write\\n\") == 19 bytes");
    check((i64)r[23] >= 0, "ring3: NxDuplicateHandle(stdout) succeeded");
    check(r[24] == 24, "ring3: NxWrite(dup(stdout), ...) == 24 bytes");
    check(r[25] == 0, "ring3: NxClose(duplicated stdout) == 0");

    /* --- 프로세스 정보 --- */
    check(r[26] == 0, "ring3: NxProcessInfo(SELF) == 0");
    {
        struct nx_procinfo pi;

        memcpy(&pi, (const void *)(r + (0x190 / 8)), sizeof(pi));
        check(pi.pid != 0, "ring3: NxProcessInfo pid != 0 (idle thread is pid 0)");
        check(pi.state == PROCESS_RUNNING, "ring3: NxProcessInfo state == RUNNING (queried about itself while running)");
        check(pi.handle_count >= 3U, "ring3: NxProcessInfo handle_count includes stdin/stdout/stderr");
    }

    /* --- NxProcessCreate 가 알 수 없는(존재하지 않는) 실행 파일을 거부하는지 (syscall 경로) --- */
    check((i64)r[27] < 0, "ring3: NxProcessCreate(nonexistent path) via syscall is rejected");

    (void)pfa_free(data_phys);

    run_user_program(user_prog2_start, user_prog2_end, &r, &data_phys, &code);
    check(live_processes() == 1, "user-mode #GP (hlt) terminated only the user process; kernel survived");
    check(code == -1, "a process killed by a user-mode fault reports exit code -1");
    if (data_phys)
        (void)pfa_free(data_phys);

    process_reap_terminated();   /* 두 테스트 프로세스의 전용 주소 공간을 즉시 회수 */
}

/*
 * NxProcessCreate 전체 경로 검증: 실제 ELF64 실행 파일(app:/elftest.run, 별도로 빌드되어
 * initrd 에 들어 있다 — userland/elftest/)을 열고, 검증하고, 새 주소 공간에 세그먼트를
 * 매핑하고, 64KiB 스택(+가드 페이지)을 만들고, ring3 로 실행한다. 그 프로그램 자신이
 * .data/.bss/.rodata/스택/파일 읽기/힙(brk 늘리기·줄이기·재확장)을 내부적으로 검사하고
 * 종료 코드로 결과를 알려준다 (userland/elftest/elftest.c 참고).
 *
 * 또한 process_spawn() 이 실패하는 입력(존재하지 않는 파일, ELF 가 아닌 파일)에서 프레임을
 * 하나도 새어나가게 하지 않는지, 그리고 성공한 프로세스가 완전히 끝난 뒤 process_reap() 이
 * 전용 주소 공간(ELF 세그먼트 + 힙 + 스택 + 페이지 테이블 전부)을 물리 프레임 할당자에
 * 정확히 돌려주는지 — 즉 "1-2. 프로세스별 독립 주소공간 / 프로세스 종료 시 페이지테이블 회수"
 * 요구사항을 자체 점검으로 직접 확인한다.
 */
static void spawn_tests(void)
{
    static const char elftest_args[] = "elftest\0main\0NYX_TEST=1\0LANG=ko";
    spawn_args_t args;
    process_t *proc = nNULL;
    u32 pid = 0;
    i32 code = 0;
    u64 pfa_before;
    u64 pfa_after;
    Nstatus status;
    int waited;

    printk("SELFTEST: NxProcessCreate (ELF loader, address spaces, heap, argv/envp, process handles, preemption, stdin)\n");

    args.argc = 2;
    args.envc = 2;
    args.size = sizeof(elftest_args);       /* 마지막 NUL 포함 */
    args.strings = elftest_args;

    process_reap_terminated();
    pfa_before = pfa_free_count();

    status = process_spawn("app:/nonexistent.run", nNULL, &proc);
    check(NSTATUS_IS_ERR(status), "process_spawn() rejects a missing executable");

    status = process_spawn("app:/hellowld.run", nNULL, &proc);   /* 존재하지만 ELF 가 아닌 평문 파일 */
    check(NSTATUS_IS_ERR(status), "process_spawn() rejects a non-ELF file");

    check(pfa_free_count() == pfa_before, "rejected process_spawn() attempts leaked no frames");

    {
        static const char bad1[] = "a\0b";   /* argc+envc=3 라고 했지만 문자열은 2 개 */
        spawn_args_t bad;

        bad.argc = 2;
        bad.envc = 1;
        bad.size = sizeof(bad1);
        bad.strings = bad1;
        check(process_spawn("app:/elftest.run", &bad, &proc) == NinvalidArg,
              "process_spawn() rejects malformed argv/envp (string count mismatch)");
        bad.argc = (u32)NX_ARGV_MAX + 1U;
        bad.envc = 0;
        check(process_spawn("app:/elftest.run", &bad, &proc) == Noverflow,
              "process_spawn() rejects more than NX_ARGV_MAX arguments");
    }

    status = process_spawn("app:/elftest.run", &args, &proc);
    check(status == Nok, "process_spawn(app:/elftest.run, argv={elftest,main}, envp={NYX_TEST=1,LANG=ko}) succeeded");

    if (status == Nok) {
        pid = proc->pid;

        /* 끝날 때까지 기다린다. elftest 의 "stdin" 자식은 NxRead(stdin) 에서 잠들어 있으므로,
         * 잠시 뒤 키 입력을 흉내 내 "ping\n" 과 Ctrl-D(EOF)를 넣어 준다 (실제 IRQ 핸들러와 같은 경로). */
        for (waited = 0; waited < 600 && live_processes() > 1; waited++) {
            if (waited == 20) {
                const char *keys = "ping\n\x04";

                while (*keys)
                    tty_input_char(*keys++);
            }
            schedule();
            sleep_ms(5);
        }
        check(waited < 600, "elftest and all its children finished within ~3s (incl. a killed busy-looping child)");

        /* 주소 공간은 종료 즉시 회수되지만, 참조(핸들)가 남아 있는 동안 슬롯과 종료 코드는 남는다 */
        process_reap_terminated();
        check(pfa_free_count() == pfa_before,
              "terminated processes' address spaces were reclaimed while their slots were still referenced");
        check(process_wait(proc, 0, &code) == Nok, "exit code is readable while a reference is held");
        printk("  elftest exit code: %d\n", code);
        check(code == 42,
              "elftest exited with code 42 (ELF, stack, heap, files, syscall instr, argv/envp, NxProcessCreate/Wait/Kill, preemption, stdin)");
        check(process_wait(proc, 0, &code) == Nok && code == 42, "exit code can be read again (not consumed)");

        process_release(proc);
        check(count_slots() == 1, "releasing the last reference freed every process slot (incl. a child whose handle elftest never closed)");
        (void)pid;
    }

    process_reap_terminated();
    pfa_after = pfa_free_count();
    check(pfa_after == pfa_before,
          "process teardown returned every frame to the allocator (address space + heap + stack, no leak)");
}

/* 커널 힙: 크기별 할당, 정렬, 겹침 없음, kzalloc, 빈 페이지 반환 */
static void kheap_tests(void)
{
    void *p[64];
    usize sz[64];
    u64 pfa0 = pfa_free_count();
    void *pg;
    u32 i;
    usize j;
    bool ok = true;

    printk("SELFTEST: kernel heap (kmalloc/kfree/kpage)\n");

    check(kmalloc(0) == nNULL && kmalloc(KHEAP_MAX_ALLOC + 1) == nNULL, "kmalloc rejects 0 and > KHEAP_MAX_ALLOC bytes");

    for (i = 0; i < 64; i++) {
        sz[i] = ((usize)16U << (i % 8U)) - (usize)(i % 3U);
        p[i] = kmalloc(sz[i]);
        if (!p[i] || ((u64)(usize)p[i] & 15UL) != 0)
            ok = false;
        else
            memset(p[i], (int)i, sz[i]);
    }
    check(ok, "kmalloc 64 objects across all size classes (16-byte aligned)");

    ok = true;
    for (i = 0; i < 64; i++) {
        for (j = 0; p[i] && j < sz[i]; j++) {
            if (((u8 *)p[i])[j] != (u8)i)
                ok = false;
        }
    }
    check(ok, "kmalloc objects do not overlap (every byte kept its pattern)");

    for (i = 0; i < 64; i += 2)
        kfree(p[i]);
    ok = true;
    for (i = 0; i < 64; i += 2) {
        p[i] = kzalloc(sz[i]);
        for (j = 0; p[i] && j < sz[i]; j++) {
            if (((u8 *)p[i])[j] != 0)
                ok = false;
        }
        if (!p[i])
            ok = false;
    }
    check(ok, "kzalloc returns zeroed memory even when reusing freed slots");

    for (i = 0; i < 64; i++)
        kfree(p[i]);
    check(kheap_live_objects() == 0 && kheap_slab_pages() == 0, "kfree of everything leaves no live objects and no slab pages");
    check(pfa_free_count() == pfa0, "empty slab pages were returned to the frame allocator");

    pg = kpage_alloc();
    check(pg && ((u64)(usize)pg & 4095UL) == 0 && ((u8 *)pg)[0] == 0 && ((u8 *)pg)[4095] == 0,
          "kpage_alloc returns a zeroed, page-aligned page");
    kpage_free(pg);
    check(pfa_free_count() == pfa0, "kpage_free returned the page");
}

/* 대기 큐: 깨울 때까지 잠들기, 시간 제한, idle 은 잠들 수 없음 */
static waitq_t g_test_wq;
static waitq_t g_test_wq_timeout;
static volatile i32 g_wq_woken = 99;
static volatile i32 g_wq_timed = 99;

static void wq_waiter_thread(void *arg)
{
    u64 flags = irq_save();

    (void)arg;
    g_wq_woken = (i32)waitq_sleep_locked(&g_test_wq, 0);
    irq_restore(flags);
}

static void wq_timeout_thread(void *arg)
{
    u64 flags = irq_save();

    (void)arg;
    g_wq_timed = (i32)waitq_sleep_locked(&g_test_wq_timeout, 3);
    irq_restore(flags);
}

static void waitq_tests(void)
{
    u64 flags;
    u32 i;

    printk("SELFTEST: wait queues\n");

    waitq_init(&g_test_wq);
    waitq_init(&g_test_wq_timeout);

    flags = irq_save();
    check(waitq_sleep_locked(&g_test_wq, 1) == Npermission, "idle/boot thread cannot sleep on a wait queue");
    irq_restore(flags);

    check(process_create(wq_waiter_thread, nNULL) == Nok && process_create(wq_timeout_thread, nNULL) == Nok,
          "create two waiting kernel threads");
    for (i = 0; i < 10; i++)
        schedule();
    check(g_wq_woken == 99, "a waiter stays asleep until woken (no busy return)");

    check(waitq_wake_all(&g_test_wq) == 1, "waitq_wake_all woke exactly the one waiter");
    for (i = 0; i < 10; i++)
        schedule();
    check(g_wq_woken == (i32)Nok, "woken waiter returned Nok");

    for (i = 0; i < 50 && g_wq_timed == 99; i++) {
        schedule();
        sleep_ms(10);
    }
    check(g_wq_timed == (i32)Ntimeout, "a waiter with a timeout returns Ntimeout when nobody wakes it");
    check(waitq_wake_all(&g_test_wq_timeout) == 0, "a timed-out waiter is no longer on the queue");
}

#if NYX_SELFTEST == 4 || NYX_SELFTEST == 8
static unsigned long recurse(unsigned long n)
{
    volatile char pad[512];

    pad[0] = (char)n;
    if (n == 0xFFFFFFFFFFFFFFFFUL)      /* 컴파일러의 무한 재귀 경고/제거를 피하기 위한 (도달 불가) 종료 조건 */
        return 0;
    return recurse(n + 1) + (unsigned long)pad[0];
}
#endif

#if NYX_SELFTEST == 8
static void overflow_thread(void *arg)
{
    (void)arg;
    printk("%lu\n", recurse(0));
}
#endif

static void functional_tests(void)
{
    u32 i;
    u64 t0;
    u64 t1;
    void *vaddr = (void *)0x180000000UL;   /* 커널 전용 주소 공간의 사용자 영역 안 임의 주소 */
    addr_space_t kernel_as = paging_kernel_addrspace();

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

    /* --- 페이징 API + 유저 포인터 검증 (idle 스레드가 쓰는 "커널 전용" 주소 공간에 매핑:
     * paging_kernel_addrspace() 문서 참고) ---
     * NxKernelPrint 를 통한 실제 uaccess 검증은 current_process->addrspace 를 보므로,
     * idle 스레드(addrspace.pml4 == NULL, 즉 "이 프로세스에 속한 사용자 메모리는 없음")로는
     * 통과할 수 없는 게 맞다. 이 아래 블록만 잠깐 idle 의 addrspace 를 kernel_as 로 바꿔서
     * "이 주소 공간을 가진 프로세스가 시스템 콜을 했다면" 을 흉내 낸다 (스케줄링이 끼어들지
     * 않는 동기 구간이므로 안전하다 — 이 함수는 schedule()/sleep_ms() 를 부르지 않는다). */
    memset(g_scratch_page, 0, sizeof(g_scratch_page));
    strcpy((char *)g_scratch_page, "hello from a user-mapped page via %s %x\n");

    check(paging_map_page(&kernel_as, (void *)(usize)virt_to_phys(g_scratch_page), vaddr,
                          PAGE_RW | PAGE_USER | PAGE_NX) == Nok, "paging_map_page(RW|USER|NX)");
    check(paging_map_page(&kernel_as, (void *)(usize)virt_to_phys(g_scratch_page), vaddr,
                          PAGE_USER) == NalreadyExists, "paging_map_page duplicate -> NalreadyExists");
    check(paging_map_page(&kernel_as, (void *)(usize)virt_to_phys(g_scratch_page), (void *)0x181000000UL,
                          PAGE_RW | PAGE_USER) == Npermission, "paging_map_page RW+X rejected (W^X)");
    check(paging_is_user_range(&kernel_as, vaddr, 4096, true), "paging_is_user_range(mapped, write)");
    check(!paging_is_user_range(&kernel_as, vaddr, 4097, false), "paging_is_user_range spanning unmapped page rejected");
    check(!paging_is_user_range(&kernel_as, (void *)0x1000UL, 16, false), "paging_is_user_range(kernel/identity) rejected");

    current_process->addrspace = kernel_as;
    check(sys(NX_SYS_KERNEL_PRINT, (u64)(usize)vaddr) == 0, "syscall 771 with user page (format string not expanded above)");
    current_process->addrspace.pml4 = nNULL;

    check(paging_unmap_page(&kernel_as, vaddr) == Nok, "paging_unmap_page");
    check(!paging_is_user_range(&kernel_as, vaddr, 1, false), "unmapped page no longer user-accessible");
    check(sys(NX_SYS_KERNEL_PRINT, (u64)(usize)vaddr) == (i64)NinvalidPointer, "syscall 771 after unmap rejected");

    /* --- 커널 스레드 컨텍스트 스위치 --- */
    check(process_create(thread_a, nNULL) == Nok, "process_create(thread_a)");
    check(process_create(thread_b, nNULL) == Nok, "process_create(thread_b)");
    {
        process_t *p;
        int guarded = 0;

        for (p = process_list; p; p = p->next) {
            if (p->pid != 0 && p->kernel_stack &&
                !paging_kernel_page_mapped((const u8 *)p->kernel_stack - PAGE_SIZE) &&
                paging_kernel_page_mapped(p->kernel_stack))
                guarded++;
        }
        check(guarded == 2, "kernel stacks have an unmapped guard page below them");
    }
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

    kheap_tests();
    waitq_tests();
    ring3_tests();
    spawn_tests();

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
    g_scratch_page[0] = 0xC3;              /* ret */
    __asm__ volatile ("call *%0" : : "r"(g_scratch_page) : "memory");
#elif NYX_SELFTEST == 7
    printk("SELFTEST: triggering #UD\n");
    __asm__ volatile ("ud2");
#elif NYX_SELFTEST == 9
    {
        void *keep = kmalloc(32);       /* 같은 slab 페이지를 살려 두어 "진짜" 이중 해제 검출 경로를 탄다 */
        void *p = kmalloc(32);

        printk("SELFTEST: double kfree (expect panic)\n");
        kfree(p);
        kfree(p);
        kfree(keep);
    }
#elif NYX_SELFTEST == 8
    printk("SELFTEST: overflowing a kernel thread's stack (expect #DF via guard page)\n");
    (void)process_create(overflow_thread, nNULL);
    schedule();
#endif
    (void)g_scratch_page;
    (void)functional_tests;
    (void)kernel_panic_simple;
}
