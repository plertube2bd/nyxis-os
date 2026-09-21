/*
 * mb2_test.c - Multiboot2 -> NTBLI 어댑터 호스트 테스트 (정상 입력 + 무작위 변이 퍼징, ASan/UBSan)
 *
 * MBI 는 부트로더가 넘기는 "외부 입력" 이므로, 손상된 태그 크기/엔트리 크기/총 크기에서도
 * 범위 밖 접근이나 무한 루프 없이 오류를 반환해야 한다.
 * 어댑터가 4GiB 미만 물리 주소만 허용하므로 MAP_32BIT 로 낮은 주소에 MBI 를 만든다.
 * (MBI 헤더의 total_size 는 신뢰하므로, 변이로 커진 값이 매핑을 벗어나지 않도록 상한(1MiB)+여유만큼 매핑한다)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "nyxis.h"
#include "boot_info.h"
#include "kernel/boot/multiboot2.h"

bool multicore_enabled = false;

static unsigned char *g_mbi;
static size_t g_len;

static void put32(size_t off, unsigned v) { memcpy(g_mbi + off, &v, 4); }
static void put64(size_t off, unsigned long v) { memcpy(g_mbi + off, &v, 8); }

/* mmap 1개 + module 1개 + framebuffer 1개 + end 태그로 이루어진 정상 MBI 를 만든다 */
static void build_valid(void)
{
    size_t off = 8;

    memset(g_mbi, 0, 4096);

    put32(off, 6); put32(off + 4, 16 + 24 * 2); put32(off + 8, 24); put32(off + 12, 0);       /* mmap */
    put64(off + 16, 0);          put64(off + 24, 0x9F000);   put32(off + 32, 1); put32(off + 36, 0);
    put64(off + 40, 0x100000);   put64(off + 48, 0x3FF00000); put32(off + 56, 1); put32(off + 60, 0);
    off += 16 + 48;

    put32(off, 3); put32(off + 4, 24); put32(off + 8, 0x2000000); put32(off + 12, 0x3000000);  /* module */
    off += 24;

    put32(off, 8); put32(off + 4, 38);                                                        /* framebuffer */
    put64(off + 8, 0xFD000000UL); put32(off + 16, 4096); put32(off + 20, 1024); put32(off + 24, 768);
    g_mbi[off + 28] = 32; g_mbi[off + 29] = 1;
    g_mbi[off + 32] = 16; g_mbi[off + 33] = 8; g_mbi[off + 34] = 8; g_mbi[off + 35] = 8;
    g_mbi[off + 36] = 0;  g_mbi[off + 37] = 8;
    off += 40;

    put32(off, 0); put32(off + 4, 8);                                                         /* end */
    off += 8;
    put32(0, (unsigned)off); put32(4, 0);
    g_len = off;
}

static int g_fail;
static void check(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? " OK " : "FAIL", what);
    if (!cond) g_fail++;
}

int main(int argc, char **argv)
{
    static unsigned char store[4096];
    NTBLI info;
    unsigned iter = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 10) : 20000;
    unsigned i;
    unsigned ok = 0, rejected = 0;
    unsigned long seed = 12345;

    g_mbi = mmap(0, (1UL << 20) + 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (g_mbi == MAP_FAILED) { perror("mmap"); return 2; }

    printf("multiboot2 adapter tests\n");
    build_valid();
    check(multiboot2_to_ntbli((unsigned long)g_mbi, &info, store, sizeof(store)) == NSTATUS_OK, "valid MBI accepted");
    check(info.memory_size == 0x40000000UL, "memory_size = end of last available region");
    check(info.initrd_base == (void *)0x2000000UL && info.initrd_size == 0x1000000UL, "module -> initrd");
    check(info.framebuffer_base == (void *)0xFD000000UL && info.width == 1024 && info.height == 768 &&
          info.pixels_per_scan_line == 1024 && info.pixel_format == NTBLI_PIXEL_BGR, "framebuffer tag -> NTBLI (BGR, stride 1024)");
    check(info.memmap_desc_size == sizeof(ntbli_memdesc_t) && info.memmap_size == 2 * sizeof(ntbli_memdesc_t), "2 memory map entries converted");

    put32(0, 8);
    check(multiboot2_to_ntbli((unsigned long)g_mbi, &info, store, sizeof(store)) != NSTATUS_OK, "total_size < 16 rejected");
    build_valid();
    check(multiboot2_to_ntbli(((unsigned long)g_mbi) | 1, &info, store, sizeof(store)) != NSTATUS_OK, "misaligned MBI rejected");
    check(multiboot2_to_ntbli(0x100000000UL, &info, store, sizeof(store)) != NSTATUS_OK, "MBI above 4GiB rejected");

    for (i = 0; i < iter; i++) {
        unsigned n = 1 + (unsigned)(rand() % 6);
        unsigned k;

        build_valid();
        srand((unsigned)(seed + i));
        for (k = 0; k < n; k++) {
            size_t pos = (size_t)(rand() % (int)g_len);

            if (rand() & 1) g_mbi[pos] = (unsigned char)rand();
            else g_mbi[pos] ^= (unsigned char)(1U << (rand() % 8));
        }
        if (multiboot2_to_ntbli((unsigned long)g_mbi, &info, store, sizeof(store)) == NSTATUS_OK) ok++; else rejected++;
    }
    printf("fuzz: %u iterations, %u accepted, %u rejected, no crash\n", iter, ok, rejected);

    printf(g_fail ? "TEST FAILED\n" : "TEST PASSED\n");
    return g_fail ? 1 : 0;
}
