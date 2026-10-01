/*
 * elftest.c - NxProcessCreate / ELF 로더 / 프로세스별 주소 공간 / 힙 / 스택 검증용 사용자 프로그램
 *
 * 커널 자체 점검(selftest)이 이 프로그램을 app:/elftest.run 으로 실행하고 종료 코드가 42 인지
 * 확인한다. 실패하면 실패한 검사 번호를 종료 코드로 돌려준다 (1..99). 표준 라이브러리 없이
 * 시스템 콜 래퍼(nyxstd.h)만 쓴다.
 */
#include <nyxstd.h>
#include <nyx_abi.h>

#define PASS_CODE 42

static long sc(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2)
{
    return (long)syscall_wrapper(nr, a0, a1, a2, 0, 0, 0);
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

int elftest_main(long argc, char **argv);

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

    /* 1. 초기 스택 규약: argc=0, argv[0]=NULL */
    if (argc != 0)
        return 1;
    if (argv[0] != 0)
        return 2;

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

    say("elftest: ALL PASS\n");
    return PASS_CODE;
}
