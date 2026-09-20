/*
 * fat16_test.c - FAT16/VFS/램디스크 호스트 테스트 (리눅스 유저 공간에서 실행)
 *
 * 목적:
 *   1) 회귀 테스트: 과거에 발견된 버그(마운트 후 read 함수 포인터 소거, 오프셋 이중 증가,
 *      8.3 이름 잘림 매칭, app 네임스페이스 등록 실패)가 다시 나타나지 않는지 확인.
 *   2) 변이(mutation) 퍼징: initrd 이미지의 메타데이터(부트 섹터/FAT/디렉터리)를 무작위로
 *      깨뜨린 뒤 마운트 -> 열기 -> 읽기를 수행한다. 커널 코드는 ASan/UBSan 으로 빌드되므로
 *      버퍼 오버플로/미정렬 접근/정수 오버플로가 있으면 즉시 비정상 종료한다.
 *      각 시도는 fork() 된 자식 프로세스에서 실행되고 alarm 으로 무한 루프도 검출한다.
 *
 * 사용: fat16_test <initrd.img> [반복 횟수] [seed]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "nyxis.h"
#include "drivers/ramdisk/ramdisk.h"
#include "drivers/filesystem/vfs.h"
#include "drivers/filesystem/fat16/fat16.h"

/* 커널이 정의하는 전역 (kernel.c) */
bool multicore_enabled = false;

/* 자식 프로세스의 정상 종료 코드 */
#define EXIT_REJECTED 0
#define EXIT_MOUNTED  10

/* initrd 이미지의 메타데이터 영역 (mkfs.vfat -F 16, 16MiB 기준) */
struct range { size_t off; size_t len; };
static const struct range g_meta[] = {
    { 0,            512 },               /* 부트 섹터 */
    { 4 * 512,      2 * 32 * 512 },      /* FAT 2개 */
    { 68 * 512,     32 * 512 },          /* 루트 디렉터리 */
    { 100 * 512,    3 * 2048 },          /* 데이터 영역 앞부분(서브디렉터리 + 파일) */
};
#define META_COUNT (sizeof(g_meta) / sizeof(g_meta[0]))

static unsigned char *g_img;
static size_t g_size;
static NTBLI g_info;

static const char k_expected[] = "Hello from app:/hellowld.run\n";

static unsigned long g_rng;
static unsigned long rnd(void)
{
    g_rng = g_rng * 6364136223846793005UL + 1442695040888963407UL;
    return g_rng >> 33;
}

static int g_fail;
static void check(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? " OK " : "FAIL", what);
    if (!cond)
        g_fail++;
}

/* 램디스크 + FAT16 을 마운트한다. (프로세스마다 처음 한 번만 호출) */
static Nstatus mount_image(void)
{
    fat16_mount_params_t p;
    Nstatus s;

    memset(&g_info, 0, sizeof(g_info));
    g_info.magic = NTBLI_MAGIC;
    g_info.version = NTBLI_VERSION;
    g_info.size = sizeof(NTBLI);
    g_info.memory_size = 0x7FFFFFFFFFFFUL;      /* 호스트 주소 공간 전체를 "RAM" 으로 간주 */

    s = vfs_init();
    if (NSTATUS_IS_ERR(s)) return s;
    s = fat16_register();
    if (NSTATUS_IS_ERR(s)) return s;
    s = ramdisk_init(0, (usize)g_img, g_size, &g_info);
    if (NSTATUS_IS_ERR(s)) return s;

    p.mount_path = "initrd:/";
    p.info = &g_info;
    p.read = ramdisk_read;
    p.write = 0;
    s = fat16_mount(0, &p);
    if (NSTATUS_IS_ERR(s)) return s;

    s = vfs_register_namespace("app", "initrd:/_ns_/app");
    return s;
}

/* 손상된 이미지에서도 죽지 않아야 하는 일련의 동작 */
static void exercise(void)
{
    static const char *paths[] = {
        "app:/hellowld.run", "app:/HELLOWLD.RUN", "initrd:/_ns_/app/hellowld.run",
        "initrd:/_ns_", "initrd:/", "app:/nonexistent.txt", "app:/a/b/c/d", "initrd:/_ns_/app",
        "app:/hellowld1.run"
    };
    unsigned i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        handle_t h;
        char buf[64];
        usize got = 0;

        if (!NSTATUS_IS_ERR(vfs_open(paths[i], 0, &h))) {
            (void)vfs_read(&h, buf, sizeof(buf), &got);
            (void)vfs_read(&h, buf, 3, &got);
            (void)vfs_close(&h);
        }
    }
}

static int regression_tests(void)
{
    handle_t h;
    char buf[128];
    usize got = 0;
    Nstatus s;

    printf("regression tests\n");

    s = mount_image();
    check(!NSTATUS_IS_ERR(s), "mount + vfs_register_namespace(app -> initrd:/_ns_/app)");

    s = vfs_open("app:/hellowld.run", 0, &h);
    check(!NSTATUS_IS_ERR(s), "open app:/hellowld.run");

    memset(buf, 0, sizeof(buf));
    s = vfs_read(&h, buf, 5, &got);
    check(!NSTATUS_IS_ERR(s) && got == 5 && memcmp(buf, "Hello", 5) == 0, "first partial read = 'Hello'");
    s = vfs_read(&h, buf, 5, &got);
    check(!NSTATUS_IS_ERR(s) && got == 5 && memcmp(buf, k_expected + 5, 5) == 0,
          "second partial read continues at offset 5 (no offset double-advance)");
    s = vfs_read(&h, buf, sizeof(buf), &got);
    check(!NSTATUS_IS_ERR(s) && got == strlen(k_expected) - 10, "third read returns the remaining bytes");
    s = vfs_read(&h, buf, sizeof(buf), &got);
    check(!NSTATUS_IS_ERR(s) && got == 0, "read at EOF returns 0 bytes");
    check(!NSTATUS_IS_ERR(vfs_close(&h)), "close");

    check(NSTATUS_IS_ERR(vfs_open("app:/hellowld1.run", 0, &h)),
          "9-character name is not silently truncated to match hellowld.run");
    check(NSTATUS_IS_ERR(vfs_open("app:/.run", 0, &h)), "empty base name rejected");
    check(NSTATUS_IS_ERR(vfs_open("app:/a.b.c", 0, &h)), "double dot rejected");
    check(!NSTATUS_IS_ERR(vfs_open("app:/HELLOWLD.RUN", 0, &h)), "lookup is case-insensitive");
    (void)vfs_close(&h);
    check(vfs_open("initrd:/_ns_/app", 0, &h) == Nunsupported, "opening a directory as a file -> Nunsupported");

    return g_fail;
}

/* 한 번의 변이 시도: 자식 프로세스에서 실행되며 정상 종료(0) 해야 통과 */
static void fuzz_child(unsigned long seed)
{
    unsigned nmut;
    unsigned i;

    g_rng = seed;
    nmut = 1 + (unsigned)(rnd() % 8);

    for (i = 0; i < nmut; i++) {
        const struct range *r = &g_meta[rnd() % META_COUNT];
        size_t pos = r->off + (size_t)(rnd() % r->len);

        switch (rnd() % 4) {
        case 0:  g_img[pos] = (unsigned char)rnd(); break;             /* 임의 바이트 */
        case 1:  g_img[pos] = 0xFF; break;
        case 2:  g_img[pos] = 0x00; break;
        default: g_img[pos] ^= (unsigned char)(1U << (rnd() % 8)); break;   /* 비트 반전 */
        }
    }

    alarm(5);                       /* 5초 안에 끝나지 않으면 무한 루프로 간주 */
    if (!NSTATUS_IS_ERR(mount_image())) {
        exercise();
        _exit(EXIT_MOUNTED);        /* 마운트 성공 후 동작까지 수행 */
    }
    _exit(EXIT_REJECTED);           /* 손상을 감지하고 마운트를 거부 */
}

static int fuzz(unsigned iterations, unsigned long seed)
{
    unsigned i;
    unsigned crashes = 0;
    unsigned hangs = 0;
    unsigned mounted_ok = 0;
    unsigned rejected = 0;

    printf("mutation fuzzing: %u iterations, seed %lu\n", iterations, seed);

    for (i = 0; i < iterations; i++) {
        pid_t pid = fork();
        int st = 0;

        if (pid == 0)
            fuzz_child(seed * 1000003UL + i);

        waitpid(pid, &st, 0);
        if (WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM) {
            hangs++;
            printf("  HANG  at iteration %u (reproduce: seed %lu iteration %u)\n", i, seed, i);
        } else if (!WIFEXITED(st) || (WEXITSTATUS(st) != EXIT_REJECTED && WEXITSTATUS(st) != EXIT_MOUNTED)) {
            crashes++;
            printf("  CRASH at iteration %u (reproduce: seed %lu iteration %u)\n", i, seed, i);
        } else if (WEXITSTATUS(st) == EXIT_MOUNTED) {
            mounted_ok++;
        } else {
            rejected++;
        }
    }

    printf("fuzz result: %u mounted+exercised, %u rejected as corrupt, %u crashes, %u hangs\n",
           mounted_ok, rejected, crashes, hangs);
    return (int)(crashes + hangs);
}

int main(int argc, char **argv)
{
    FILE *f;
    unsigned iterations = 1000;
    unsigned long seed = 1;
    pid_t pid;
    int st = 0;
    int bad;

    if (argc < 2) {
        fprintf(stderr, "usage: %s initrd.img [iterations] [seed]\n", argv[0]);
        return 2;
    }
    if (argc > 2) iterations = (unsigned)strtoul(argv[2], 0, 10);
    if (argc > 3) seed = strtoul(argv[3], 0, 10);

    f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 2; }
    fseek(f, 0, SEEK_END);
    g_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_img = malloc(g_size);
    if (!g_img || fread(g_img, 1, g_size, f) != g_size) { fprintf(stderr, "read failed\n"); return 2; }
    fclose(f);

    /* 회귀 테스트는 별도 프로세스에서 (VFS 전역 상태 오염 방지) */
    fflush(stdout);
    pid = fork();
    if (pid == 0)
    {
        int failed = regression_tests();

        fflush(stdout);
        _exit(failed ? 1 : 0);
    }
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("regression tests FAILED\n");
        return 1;
    }

    bad = fuzz(iterations, seed);
    printf(bad ? "TEST FAILED\n" : "TEST PASSED\n");
    return bad ? 1 : 0;
}
