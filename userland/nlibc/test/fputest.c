/*
 * fputest.c - 커널의 프로세스별 FPU/SSE 상태 저장 검사 (Nyxis 전용, app:/fputest.run)
 *
 *   fputest            부모: 자신을 일꾼 3개로 더 띄우고 자기도 일꾼 0 으로 일한 뒤 결과를 모은다
 *   fputest worker <n> 일꾼 n
 *
 * 일꾼은 시간 동안(약 1.5초, 타임슬라이스 수십 번) 다음을 되풀이한다:
 *   - xmm0~xmm15 에 일꾼마다 다른 값을 넣고, 수백만 번 다시 읽어 그대로인지 본다 (asm)
 *   - MXCSR 반올림 모드를 일꾼마다 다르게 두고, 1/3 의 마지막 비트로 모드가 유지되는지 본다
 *   - x87 (long double) 누적 합이 정확한지 본다
 * 커널이 전환 때 상태를 저장/복원하지 않으면 다른 일꾼의 값이 섞여 실패한다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <nyx_abi.h>

static long sc4(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3)
{
    unsigned long ret;
    register unsigned long r10_ __asm__("r10") = a3;

    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10_)
                      : "rcx", "r11", "memory");
    return (long)ret;
}

static long now_ms(void)
{
    return sc4(NX_SYS_GET_TIME, NX_CLOCK_MONOTONIC, 0, 0, 0) / 1000000L;
}

/* xmm0..15 에 pattern+k 를 넣고 loops 번 다시 확인. 반환: 0 성공, 1 값이 바뀜 */
static int xmm_check(unsigned long pattern, unsigned long loops)
{
    unsigned long bad;

    __asm__ volatile (
        "movq %1, %%rax\n\t"
        "movq %%rax, %%xmm0\n\t  incq %%rax\n\t movq %%rax, %%xmm1\n\t  incq %%rax\n\t"
        "movq %%rax, %%xmm2\n\t  incq %%rax\n\t movq %%rax, %%xmm3\n\t  incq %%rax\n\t"
        "movq %%rax, %%xmm4\n\t  incq %%rax\n\t movq %%rax, %%xmm5\n\t  incq %%rax\n\t"
        "movq %%rax, %%xmm6\n\t  incq %%rax\n\t movq %%rax, %%xmm7\n\t  incq %%rax\n\t"
        "movq %%rax, %%xmm8\n\t  incq %%rax\n\t movq %%rax, %%xmm9\n\t  incq %%rax\n\t"
        "movq %%rax, %%xmm10\n\t incq %%rax\n\t movq %%rax, %%xmm11\n\t incq %%rax\n\t"
        "movq %%rax, %%xmm12\n\t incq %%rax\n\t movq %%rax, %%xmm13\n\t incq %%rax\n\t"
        "movq %%rax, %%xmm14\n\t incq %%rax\n\t movq %%rax, %%xmm15\n\t"
        "xorl %%ecx, %%ecx\n"
        "1:\n\t"
        "movq %1, %%rax\n\t"
        "movq %%xmm0, %%rdx\n\t  cmpq %%rax, %%rdx\n\t jne 2f\n\t incq %%rax\n\t"
        "movq %%xmm1, %%rdx\n\t  cmpq %%rax, %%rdx\n\t jne 2f\n\t incq %%rax\n\t"
        "movq %%xmm2, %%rdx\n\t  cmpq %%rax, %%rdx\n\t jne 2f\n\t incq %%rax\n\t"
        "movq %%xmm3, %%rdx\n\t  cmpq %%rax, %%rdx\n\t jne 2f\n\t incq %%rax\n\t"
        "movq %%xmm4, %%rdx\n\t  cmpq %%rax, %%rdx\n\t jne 2f\n\t incq %%rax\n\t"
        "movq %%xmm5, %%rdx\n\t  cmpq %%rax, %%rdx\n\t jne 2f\n\t incq %%rax\n\t"
        "movq %%xmm6, %%rdx\n\t  cmpq %%rax, %%rdx\n\t jne 2f\n\t incq %%rax\n\t"
        "movq %%xmm7, %%rdx\n\t  cmpq %%rax, %%rdx\n\t jne 2f\n\t incq %%rax\n\t"
        "movq %%xmm8, %%rdx\n\t  cmpq %%rax, %%rdx\n\t jne 2f\n\t incq %%rax\n\t"
        "movq %%xmm9, %%rdx\n\t  cmpq %%rax, %%rdx\n\t jne 2f\n\t incq %%rax\n\t"
        "movq %%xmm10, %%rdx\n\t cmpq %%rax, %%rdx\n\t jne 2f\n\t incq %%rax\n\t"
        "movq %%xmm11, %%rdx\n\t cmpq %%rax, %%rdx\n\t jne 2f\n\t incq %%rax\n\t"
        "movq %%xmm12, %%rdx\n\t cmpq %%rax, %%rdx\n\t jne 2f\n\t incq %%rax\n\t"
        "movq %%xmm13, %%rdx\n\t cmpq %%rax, %%rdx\n\t jne 2f\n\t incq %%rax\n\t"
        "movq %%xmm14, %%rdx\n\t cmpq %%rax, %%rdx\n\t jne 2f\n\t incq %%rax\n\t"
        "movq %%xmm15, %%rdx\n\t cmpq %%rax, %%rdx\n\t jne 2f\n\t"
        "incq %%rcx\n\t"
        "cmpq %2, %%rcx\n\t"
        "jb 1b\n\t"
        "xorl %%eax, %%eax\n\t"
        "jmp 3f\n"
        "2:\n\t"
        "movl $1, %%eax\n"
        "3:\n\t"
        "movq %%rax, %0\n\t"
        : "=r"(bad)
        : "r"(pattern), "r"(loops)
        : "rax", "rcx", "rdx", "cc",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
    return (int)bad;
}

static unsigned int get_mxcsr(void)
{
    unsigned int v;

    __asm__ volatile ("stmxcsr %0" : "=m"(v));
    return v;
}

static void set_mxcsr(unsigned int v)
{
    __asm__ volatile ("ldmxcsr %0" : : "m"(v));
}

static int worker(int id)
{
    /* MXCSR 반올림 모드 (비트 13~14): 0 가장 가까운, 1 아래로, 2 위로, 3 0 쪽으로 */
    unsigned int mode = (unsigned int)(id & 3);
    unsigned int mx = 0x1F80U | (mode << 13);
    unsigned long pattern = 0x1111111111111111UL * (unsigned long)(id + 1);
    long end = now_ms() + 1500;
    long rounds = 0;
    volatile double three = 3.0;
    double third;
    union { double d; unsigned long u; } t;
    unsigned long want_low = (mode == 2) ? 0x5556UL : 0x5555UL;   /* 1/3 = 0x3FD5555555555555 (위로면 ...56) */
    long double acc;
    long i;

    set_mxcsr(mx);
    do {
        if (xmm_check(pattern, 2000000UL)) {
            printf("fputest worker %d: xmm registers changed\n", id);
            return 1;
        }
        if ((get_mxcsr() & ~0x3FU) != mx) {     /* 비트 0~5 는 예외 상태 플래그 (계산하면 켜진다) */
            printf("fputest worker %d: MXCSR changed (%08x -> %08x)\n", id, mx, get_mxcsr());
            return 1;
        }
        t.d = 1.0 / three;
        third = t.d;
        (void)third;
        if ((t.u & 0xFFFFUL) != want_low) {
            printf("fputest worker %d: rounding mode lost (1/3 = %016lx)\n", id, t.u);
            return 1;
        }
        acc = 0.0L;                                         /* x87: 정수 합은 정확해야 한다 */
        for (i = 1; i <= 100000; i++)
            acc += (long double)(i * (id + 1));
        if (acc != (long double)(100000L * 100001L / 2L * (long)(id + 1))) {
            printf("fputest worker %d: x87 sum wrong\n", id);
            return 1;
        }
        rounds++;
    } while (now_ms() < end);
    set_mxcsr(0x1F80U);
    printf("fputest worker %d: %ld rounds ok\n", id, rounds);
    return 0;
}

int main(int argc, char **argv)
{
    long h[3];
    int i;
    int failed = 0;

    if (argc == 3 && strcmp(argv[1], "worker") == 0)
        return worker(atoi(argv[2]));

    for (i = 0; i < 3; i++) {
        char idbuf[4];
        const char *args[4];

        sprintf(idbuf, "%d", i + 1);
        args[0] = "fputest";
        args[1] = "worker";
        args[2] = idbuf;
        args[3] = NULL;
        h[i] = sc4(NX_SYS_PROCESS_CREATE, (unsigned long)"app:/fputest.run", (unsigned long)args, 0, NX_NO_TOKEN);
        if (h[i] < 0) {
            printf("fputest: cannot spawn worker (%ld)\n", h[i]);
            return 1;
        }
    }
    failed |= worker(0);
    for (i = 0; i < 3; i++) {
        int code = -1;

        (void)sc4(NX_SYS_PROCESS_WAIT, (unsigned long)h[i], NX_WAIT_INFINITE, (unsigned long)&code, 0);
        if (code != 0)
            failed = 1;
    }
    printf(failed ? "fputest: FAILED\n" : "fputest: OK\n");
    return failed;
}
