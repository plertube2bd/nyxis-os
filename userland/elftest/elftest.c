/*
 * elftest.c - NxProcessCreate / ELF 로더 / 프로세스별 주소 공간 / 힙 / 스택 검증용 사용자 프로그램
 *
 * 커널 자체 점검(selftest)이 이 프로그램을 app:/elftest.run 으로 실행하고 종료 코드가 42 인지
 * 확인한다. 실패하면 실패한 검사 번호를 종료 코드로 돌려준다 (1..99). 표준 라이브러리 없이
 * 시스템 콜 래퍼(nyxstd.h)만 쓴다.
 *
 * argv[1] 로 역할을 정한다 (자기 자신을 자식 프로세스로 다시 실행해 프로세스 API 를 검사하기 위해):
 *   "main"  : 전체 검사 (커널 selftest 가 envp = {"NYX_TEST=1", "LANG=ko"} 로 실행)
 *   "child" : 인자를 확인하고 종료 코드 7 로 끝남
 *   "stdin" : NxRead(stdin) 으로 "ping\n" 한 줄, 이어서 EOF(0 바이트)를 읽으면 종료 코드 11
 *             (입력은 커널 selftest 가 키 입력을 흉내 내 넣어 준다 — 그 전까지 이 프로세스는 잠든다)
 *   "spin"  : 사용자 모드에서 영원히 돈다 (선점과 NxProcessKill 검사용)
 */
#include <nyxstd.h>
#include <nyx_abi.h>

#define PASS_CODE 42

/* 커널 Nstatus 오류 값 (include/types.h: NSTATUS_ERR_FLAG | code, i32 를 부호 확장) */
#define ST_ERR(code)    ((long)(int)(0x80000000U | (unsigned int)(code)))
#define Npermission     ST_ERR(4)
#define Ntimeout        ST_ERR(5)
#define NinvalidArg     ST_ERR(2)

static long sc(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2)
{
    return (long)syscall_wrapper(nr, a0, a1, a2, 0, 0, 0);
}

/* `syscall` 명령 경로 (nyxstd.h 의 래퍼는 int 0x80 을 쓴다). 인자는 3개까지만 쓴다. */
static long sc_fast(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2)
{
    unsigned long ret;

    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory"
    );
    return (long)ret;
}

static unsigned long slen(const char *s)
{
    unsigned long n = 0;

    while (s[n])
        n++;
    return n;
}

static void say(const char *s)
{
    (void)sc(NX_SYS_WRITE, NX_HANDLE_STDOUT, (unsigned long)s, slen(s));
}

/* .data(초기값 있음) / .bss(0 이어야 함, 여러 페이지에 걸침) / .rodata */
static volatile int g_data = 0x1234;
static volatile unsigned char g_bss[10000];
static const char g_rodata[] = "elftest: rodata ok\n";

/* 스택 사용 검증: 큰 지역 배열이 여러 페이지에 걸쳐도 정상이어야 한다 */
static int stack_test(void)
{
    volatile unsigned char buf[20000];
    unsigned long i;

    for (i = 0; i < sizeof(buf); i += 4096)
        buf[i] = (unsigned char)(i / 4096 + 1);
    for (i = 0; i < sizeof(buf); i += 4096) {
        if (buf[i] != (unsigned char)(i / 4096 + 1))
            return 0;
    }
    return 1;
}

static int seq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static long create(const char *mode)
{
    const char *args[3];

    args[0] = "elftest";
    args[1] = mode;
    args[2] = 0;
    return (long)syscall_wrapper(NX_SYS_PROCESS_CREATE, (unsigned long)"app:/elftest.run",
                                 (unsigned long)args, 0, 0, 0, 0);
}

static long pwait(long h, unsigned long timeout_ms, int *code)
{
    return sc(NX_SYS_PROCESS_WAIT, (unsigned long)h, timeout_ms, (unsigned long)code);
}

/* "stdin" 역할: 한 줄을 읽고, 이어서 EOF 를 읽는다 */
static int stdin_child(void)
{
    char buf[64];
    long r;

    r = sc(NX_SYS_READ, NX_HANDLE_STDIN, (unsigned long)buf, sizeof(buf));
    if (r != 5 || buf[0] != 'p' || buf[3] != 'g' || buf[4] != '\n')
        return 12;
    r = sc(NX_SYS_READ, NX_HANDLE_STDIN, (unsigned long)buf, sizeof(buf));
    if (r != 0)
        return 13;
    return 11;
}

/* 프로세스 API 검사 (main 역할에서만). 실패하면 50번대 코드 */
static int process_tests(void)
{
    long h;
    long hd;
    int code = 0;
    struct nx_procinfo self;
    struct nx_procinfo pi;

    if (sc(NX_SYS_PROCESS_INFO, NX_PID_SELF, (unsigned long)&self, 0) != 0)
        return 50;

    /* 자식이 인자를 받고 종료 코드를 돌려준다. 기다리는 동안 이 프로세스는 잠든다 */
    h = create("child");
    if (h < 0)
        return 51;
    if (pwait(h, NX_WAIT_INFINITE, &code) != 0 || code != 7)
        return 52;
    if (pwait(h, 0, &code) != 0 || code != 7)
        return 53;                      /* 핸들을 닫기 전까지 종료 코드는 다시 읽을 수 있다 */
    if (sc(NX_SYS_PROCESS_INFO, (unsigned long)h, (unsigned long)&pi, 0) != 0 ||
        pi.pid == self.pid || pi.state != 3)
        return 54;
    if (sc(NX_SYS_CLOSE, (unsigned long)h, 0, 0) != 0)
        return 55;

    /* 콘솔 입력: 자식은 입력이 올 때까지 NxRead 에서 잠들어 있다가 깨어난다 */
    h = create("stdin");
    if (h < 0)
        return 56;
    if (pwait(h, NX_WAIT_INFINITE, &code) != 0 || code != 11)
        return 57;
    (void)sc(NX_SYS_CLOSE, (unsigned long)h, 0, 0);

    /* 선점 + 강제 종료: 자식은 사용자 모드에서 영원히 돈다. 선점이 없으면 아래 NxSleep 이 돌아오지 않는다 */
    h = create("spin");
    if (h < 0)
        return 58;
    if (sc(NX_SYS_SLEEP, 30, 0, 0) != 0)
        return 59;
    if (pwait(h, 20, &code) != (long)Ntimeout)
        return 60;                      /* 아직 돌고 있다 */
    hd = sc(NX_SYS_DUPLICATE_HANDLE, (unsigned long)h, NX_RIGHT_WAIT | NX_RIGHT_STAT, 0);
    if (hd < 0)
        return 61;
    if (sc(NX_SYS_PROCESS_KILL, (unsigned long)hd, 0, 0) != (long)Npermission)
        return 62;                      /* KILL 권한 없는 복제 핸들로는 죽일 수 없다 */
    if (sc(NX_SYS_PROCESS_KILL, (unsigned long)h, 0, 0) != 0)
        return 63;
    if (pwait(h, NX_WAIT_INFINITE, &code) != 0 || code != NX_EXIT_KILLED)
        return 64;
    if (pwait(hd, 0, &code) != 0 || code != NX_EXIT_KILLED)
        return 65;                      /* 복제 핸들도 같은 프로세스를 가리킨다 */
    (void)sc(NX_SYS_CLOSE, (unsigned long)h, 0, 0);
    (void)sc(NX_SYS_CLOSE, (unsigned long)hd, 0, 0);

    /* 잘못된 사용 */
    if (pwait((long)NX_HANDLE_STDOUT, 0, &code) != (long)NinvalidArg)
        return 66;                      /* 프로세스 핸들이 아니다 */
    if (sc(NX_SYS_PROCESS_CREATE, (unsigned long)"app:/nonexistent.run", 0, 0) >= 0)
        return 67;

    /* 일부러 닫지 않는 핸들: 이 프로세스가 끝나면 커널이 닫고 자식 슬롯을 회수해야 한다
     * (커널 selftest 가 "프레임/슬롯 누수 없음" 으로 확인한다) */
    if (create("child") < 0)
        return 68;

    return 0;
}

int elftest_main(long argc, char **argv);
int elftest_system_tests(void);
int elftest_user_mode(void);
int elftest_ioport_mode(void);

int elftest_main(long argc, char **argv)
{
    unsigned long i;
    unsigned long brk0;
    unsigned long brk1;
    unsigned long brk2;
    unsigned char *heap;
    long fd;
    char hello[8];
    long r;
    char **envp;

    /* 1. 초기 스택 규약: argc, argv[], NULL, envp[], NULL */
    if (argc != 2 || !argv[0] || !argv[1] || argv[2] != 0)
        return 1;
    if (!seq(argv[0], "elftest"))
        return 2;
    envp = argv + argc + 1;

    if (seq(argv[1], "child"))
        return (envp[0] == 0) ? 7 : 8;  /* 자식에게는 환경을 넘기지 않았다 */
    if (seq(argv[1], "stdin"))
        return stdin_child();
    if (seq(argv[1], "user"))
        return elftest_user_mode();     /* 토큰으로 test(uid 1000) 신원을 받아 실행된다 (systest.c) */
    if (seq(argv[1], "ioport"))
        return elftest_ioport_mode();
    if (seq(argv[1], "spin")) {
        for (;;)
            g_data++;                   /* volatile 전역: 컴파일러가 루프를 없애지 못한다 */
    }
    if (!seq(argv[1], "main"))
        return 26;
    if (!envp[0] || !seq(envp[0], "NYX_TEST=1") || !envp[1] || !seq(envp[1], "LANG=ko") || envp[2] != 0)
        return 27;
    if (envp[3] != 0 || envp[4] != 0)
        return 28;                      /* auxv: AT_NULL(0, 0) */

    /* 2. .data 초기값, .bss 0 초기화, 둘 다 쓰기 가능 */
    if (g_data != 0x1234)
        return 3;
    for (i = 0; i < sizeof(g_bss); i++) {
        if (g_bss[i] != 0)
            return 4;
    }
    g_data = 0x5678;
    for (i = 0; i < sizeof(g_bss); i += 1000)
        g_bss[i] = 0xAB;
    if (g_data != 0x5678 || g_bss[9000] != 0xAB)
        return 5;

    /* 3. .rodata 를 읽고 stdout 으로 출력 */
    if (g_rodata[0] != 'e')
        return 6;
    say(g_rodata);

    /* 4. 스택 */
    if (!stack_test())
        return 7;

    /* 5. 파일 읽기 (다른 프로세스가 아닌 "이" 프로세스의 핸들 테이블) */
    fd = sc(NX_SYS_OPEN, (unsigned long)"app:/hellowld.run", NX_O_READ, 0);
    if (fd < 0)
        return 8;
    r = sc(NX_SYS_READ, (unsigned long)fd, (unsigned long)hello, 5);
    if (r != 5 || hello[0] != 'H' || hello[4] != 'o')
        return 9;
    if (sc(NX_SYS_CLOSE, (unsigned long)fd, 0, 0) != 0)
        return 10;

    /* 6. 힙(brk): 조회, 확장(0 으로 채워져 있어야 함), 쓰기, 축소, 재확장 */
    brk0 = (unsigned long)sc(NX_SYS_VIRTUAL_ALLOC, 0, 0, 0);
    if ((long)brk0 <= 0 || (brk0 & 4095UL) != 0)
        return 11;                      /* 힙 시작은 ELF 이미지 끝 = 페이지 정렬 */
    brk1 = (unsigned long)sc(NX_SYS_VIRTUAL_ALLOC, 10000, 0, 0);
    if (brk1 != brk0 + 10000)
        return 12;
    heap = (unsigned char *)brk0;
    for (i = 0; i < 10000; i++) {
        if (heap[i] != 0)
            return 13;                  /* 새 힙은 항상 0 (다른 프로세스의 내용이 새면 안 된다) */
    }
    for (i = 0; i < 10000; i++)
        heap[i] = (unsigned char)(i * 7 + 1);
    for (i = 0; i < 10000; i++) {
        if (heap[i] != (unsigned char)(i * 7 + 1))
            return 14;
    }
    brk2 = (unsigned long)sc(NX_SYS_VIRTUAL_ALLOC, -6000, 0, 0);
    if (brk2 != brk0 + 4000)
        return 15;
    if (heap[3999] != (unsigned char)(3999 * 7 + 1))
        return 16;                      /* 남겨 둔 부분의 내용은 그대로여야 한다 */
    brk2 = (unsigned long)sc(NX_SYS_VIRTUAL_ALLOC, 8192, 0, 0);
    if (brk2 != brk0 + 12192)
        return 17;
    for (i = 8192; i < 12192; i++) {
        if (heap[i] != 0)
            return 18;                  /* 해제했다가 다시 얻은 페이지는 새로 0 으로 채워져야 한다 */
    }

    /* 7. 잘못된 요청은 거부되어야 한다 */
    if (sc(NX_SYS_VIRTUAL_ALLOC, -100000000L, 0, 0) >= 0)
        return 19;                      /* heap_start 아래로는 줄일 수 없다 */
    if (sc(NX_SYS_VIRTUAL_ALLOC, 1L << 40, 0, 0) >= 0)
        return 20;                      /* 한 번에 너무 큰 요청 */
    if ((unsigned long)sc(NX_SYS_VIRTUAL_ALLOC, 0, 0, 0) != brk2)
        return 21;                      /* 실패한 요청은 brk 를 바꾸면 안 된다 */

    /* 8. `syscall` 명령 경로: 양보(다른 프로세스로 전환됐다가 돌아옴) 전후로 계속 동작해야 한다.
     *    (예전에는 syscall 경로에서 전환/종료가 일어나면 GS 가 뒤바뀐 채 남아 다음 syscall 에서 커널 패닉) */
    if (sc_fast(NX_SYS_GET_VERSION, 0, 0, 0) != (long)NX_ABI_VERSION)
        return 22;
    if (sc_fast(NX_SYS_YIELD, 0, 0, 0) != 0)
        return 23;
    if (sc_fast(NX_SYS_GET_VERSION, 0, 0, 0) != (long)NX_ABI_VERSION)
        return 24;
    if (sc(NX_SYS_GET_VERSION, 0, 0, 0) != (long)NX_ABI_VERSION)
        return 25;                      /* int 0x80 경로와 섞어 써도 된다 */

    /* 9. 프로세스 핸들 / Wait / Kill / 선점 / 콘솔 입력 */
    r = process_tests();
    if (r != 0)
        return (int)r;

    /* 10. 파일/이름공간, 장치, 채널/NxPoll, 인증/토큰/신원 (systest.c) */
    r = elftest_system_tests();
    if (r != 0)
        return (int)r;

    say("elftest: ALL PASS\n");
    return PASS_CODE;
}
