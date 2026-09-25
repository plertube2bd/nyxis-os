/*
 * kernel.c - Nyxis 커널 메인
 *
 * [수정 이력 요약]
 *  - 부팅 순서를 재정의: 시리얼/콘솔 -> 부트 정보 검증 -> GDT/IDT/PIC -> 페이징 -> 프로세스
 *    -> 타이머 -> VFS/initrd -> AHCI. 예전에는 콘솔을 초기화하기 전에 VFS 를 먼저 만들어서
 *    그 사이에 발생한 오류/패닉이 화면에 전혀 나타나지 않았다.
 *  - NTBLI 를 "부트로더 메모리 포인터" 그대로 저장하던 것을 커널 소유 사본으로 변경하고,
 *    magic/version/size/범위를 검증한다. (NTBLI 정의 불일치 시 잘못된 값을 믿고 진행하던 문제)
 *  - Multiboot2 변환 코드(boot_info_bridge) 삭제: start.s 참고.
 *  - AHCI/프레임버퍼가 없다는 이유로 패닉하던 것을 경고 후 계속 진행으로 변경.
 *    (그래픽카드 없음/SATA 없음은 정상 구성이다. 예전엔 usb-storage 만 붙인 QEMU 에서 패닉)
 *  - "Entering ring 3 userland" 라고 출력만 하고 실제로는 hlt 루프에 있던 오해 소지 제거.
 *  - 깨진 zero_div()/error_handler 정리, 사용되지 않는 user_stack/idt 정적 배열 제거.
 *  - 인터럽트를 초기부터 꺼 두고(cli), 핸들러/타이머 준비가 끝난 뒤에만 sti 한다.
 *  - higher-half 커널: 진입 코드(boot.s)가 UEFI/Multiboot2 어느 쪽으로 부팅했는지(boot_kind)와 부트 정보의
 *    "물리 주소" 를 넘긴다. 커널은 이를 NTBLI 로 통일하고, 페이지 테이블 전환 뒤에는 주소 필드를
 *    HHDM 가상 주소로 바꿔 사용한다 (phys.h).
 */

#include "kernel/kernel.h"
#include "kernel/process/process.h"
#include "kernel/paging/paging.h"
#include "kernel/timer/pit/pit_base.h"
#include "kernel/error_handling/panic.h"
#include "console/outputs/printk.h"
#include "console/outputs/serial.h"
#include "drivers/ahci/ahci.h"
#include "drivers/ramdisk/ramdisk.h"
#include "drivers/filesystem/vfs.h"
#include "drivers/filesystem/fat16/fat16.h"
#include "drivers/filesystem/nyfs/nyfs.h"
#include "drivers/pic/pic.h"
#include "kernel/boot/multiboot2.h"
#include "kernel/syscall/syscall.h"
#include "boot_info.h"
#include "phys.h"
#include "interrupt.h"
#include "memory.h"
#include "string.h"
#include "nyxis.h"

/* 멀티코어 여부 (현재는 BSP 만 사용) */
bool multicore_enabled = false;

/* 커널 소유 부트 정보 사본 */
static NTBLI g_boot_info;
static bool g_boot_info_valid = false;

/* 메모리 맵 사본 (boot.s 의 MEMMAP_MAX 와 같은 크기). UEFI/Multiboot2 모두 여기에 담는다. */
#define MEMMAP_STORE_SIZE 65536U
static u8 g_memmap_store[MEMMAP_STORE_SIZE] __attribute__((aligned(8)));

/* boot.s 가 NTBLI 필드 오프셋을 상수로 사용하므로 레이아웃 변경을 컴파일 타임에 검출 */
NX_STATIC_ASSERT(ntbli_memmap_base_offset, __builtin_offsetof(NTBLI, memmap_base) == 72);
NX_STATIC_ASSERT(ntbli_memmap_size_offset, __builtin_offsetof(NTBLI, memmap_size) == 80);

extern u8 boot_stack_top[];

#define TIMER_HZ 100U

const NTBLI *get_kernel_info(void)
{
    return g_boot_info_valid ? &g_boot_info : (const NTBLI *)0;
}

/* 치명적 오류: 메시지를 출력하고 정지한다 */
static void fatal(Nstatus error, const char *message)
{
    printk("Kernel error: %s : %r\n", message, error);
    kernel_panic_simple(message, error);
}

/* initrd 범위(물리 주소)가 RAM 안에 있는지 확인하고, 이상하면 initrd 를 사용하지 않는다 */
static void boot_info_sanitize_initrd(void)
{
    u64 end;

    if (!g_boot_info.initrd_base && !g_boot_info.initrd_size)
        return;

    end = (u64)(usize)g_boot_info.initrd_base + g_boot_info.initrd_size;

    if (!g_boot_info.initrd_base || g_boot_info.initrd_size == 0 ||
        end < (u64)(usize)g_boot_info.initrd_base ||
        (g_boot_info.memory_size && end > g_boot_info.memory_size)) {
        g_boot_info.initrd_base = nNULL;
        g_boot_info.initrd_size = 0;
    }
}

/* UEFI(NYTB) 가 만든 NTBLI(물리 주소 in) 를 검증하고 커널 소유 메모리에 복사한다 */
static Nstatus boot_info_from_uefi(const NTBLI *in)
{
    if (!in)
        return NnullPointer;

    /* 8바이트 정렬이 아니면 구조체를 신뢰할 수 없다 */
    if (((usize)in & 7UL) != 0)
        return NinvalidPointer;

    if (in->magic != NTBLI_MAGIC)
        return NinvalidFormat;
    if (in->version != NTBLI_VERSION)
        return Nunsupported;
    if (in->size != sizeof(NTBLI))
        return NinvalidFormat;

    memcpy(&g_boot_info, in, sizeof(NTBLI));

    /* 메모리 맵은 boot.s 가 낮은 주소의 버퍼로 복사해 두었다. 커널 이미지 안의 사본으로 다시 복사 */
    if (g_boot_info.memmap_base && g_boot_info.memmap_size &&
        g_boot_info.memmap_size <= MEMMAP_STORE_SIZE) {
        memcpy(g_memmap_store, g_boot_info.memmap_base, (usize)g_boot_info.memmap_size);
        g_boot_info.memmap_base = g_memmap_store;
    } else {
        g_boot_info.memmap_base = nNULL;
        g_boot_info.memmap_size = 0;
    }

    return NSTATUS_OK;
}

/* 페이지 테이블 전환 후: 부트 정보의 물리 주소 필드를 HHDM 가상 주소로 바꾼다 */
static void boot_info_relocate(void)
{
    if (g_boot_info.initrd_base)
        g_boot_info.initrd_base = phys_to_virt((u64)(usize)g_boot_info.initrd_base);
    if (g_boot_info.framebuffer_base)
        g_boot_info.framebuffer_base = phys_to_virt((u64)(usize)g_boot_info.framebuffer_base);
    if (g_boot_info.Rsdp)
        g_boot_info.Rsdp = phys_to_virt((u64)(usize)g_boot_info.Rsdp);
}

/* 프레임버퍼 사용 가능 여부 판단 후 콘솔 초기화 (실패해도 시리얼 출력은 계속됨) */
static void console_init_from_boot_info(const NTBLI *info)
{
    u64 needed;
    Nstatus status;

    if (!info->framebuffer_base) {
        printk("No framebuffer: using serial console only\n");
        return;
    }

    if (info->pixel_format != NTBLI_PIXEL_RGB && info->pixel_format != NTBLI_PIXEL_BGR) {
        printk("Unsupported pixel format %u: framebuffer console disabled\n", info->pixel_format);
        return;
    }

    if (((usize)info->framebuffer_base & 3UL) != 0) {
        printk("Misaligned framebuffer: framebuffer console disabled\n");
        return;
    }

    /* stride*height*4 바이트가 실제 프레임버퍼 크기 안에 있는지 확인 */
    needed = (u64)info->pixels_per_scan_line * (u64)info->height * 4UL;
    if (info->framebuffer_size < needed) {
        printk("Framebuffer size mismatch: framebuffer console disabled\n");
        return;
    }

    status = printk_init((u32 *)info->framebuffer_base,
                         info->pixels_per_scan_line,
                         info->width,
                         info->height);
    if (NSTATUS_IS_ERR(status))
        printk("Framebuffer console init failed (%r): serial only\n", status);
}

/* initrd(FAT16 램디스크)를 마운트하고 app: 네임스페이스를 등록 */
static void mount_initrd(NTBLI *info)
{
    fat16_mount_params_t initrd_params;
    Nstatus status;

    status = ramdisk_init(0, (usize)info->initrd_base, (usize)info->initrd_size, info);
    if (NSTATUS_IS_ERR(status)) {
        printk("Initrd ramdisk init failed: %r\n", status);
        return;
    }

    initrd_params.mount_path = "initrd:/";
    initrd_params.info = info;
    initrd_params.read = ramdisk_read;
    initrd_params.write = nNULL;

    status = fat16_mount(0, &initrd_params);
    if (NSTATUS_IS_ERR(status)) {
        printk("Initrd FAT16 mount failed: %r\n", status);
        return;
    }

    status = vfs_register_namespace("app", "initrd:/_ns_/app");
    if (NSTATUS_IS_ERR(status)) {
        status = vfs_register_namespace("app", "initrd:/");
        if (NSTATUS_IS_ERR(status)) {
            printk("Failed to register app namespace: %r\n", status);
            return;
        }
    }

    printk("Initrd available: %lu bytes\n", (unsigned long)info->initrd_size);
}

/* 초기 사용자 프로그램 파일이 읽히는지 확인하는 간단한 점검 */
static void probe_first_app(void)
{
    handle_t file_handle;
    usize read_bytes = 0;
    char buffer[256];
    Nstatus status;

    status = vfs_open("app:/hellowld.run", 0, &file_handle);
    if (NSTATUS_IS_ERR(status)) {
        printk("app:/hellowld.run not found: %r\n", status);
        return;
    }

    status = vfs_read(&file_handle, buffer, sizeof(buffer) - 1, &read_bytes);
    if (!NSTATUS_IS_ERR(status)) {
        buffer[read_bytes] = '\0';
        printk("app:/hellowld.run (%lu bytes):\n%s\n", (unsigned long)read_bytes, buffer);
    } else {
        printk("app:/hellowld.run read failed: %r\n", status);
    }

    (void)vfs_close(&file_handle);
}

/*
 * nyfs 스모크 테스트: 커널 이미지 안에 예약해 둔 1MiB 짜리 메모리 영역을 램디스크
 * (diskno=1 - initrd 가 이미 0을 쓰고 있다)로 등록하고, 그 위에 nyfs 를 mkfs 한 뒤
 * mount, mkdir, create, write, read, readdir, unlink 를 순서대로 실제로 실행해본다.
 * 실패해도 AHCI 와 같은 방식으로 경고만 남기고 부팅은 계속한다 - 이건 어디까지나
 * "새로 만든 파일시스템이 최소한 동작은 한다"를 확인하는 용도다.
 */
#define NYFS_TEST_RAMDISK_DISKNO  1U
#define NYFS_TEST_RAMDISK_SIZE    (1024U * 1024U)   /* 1 MiB */
#define NYFS_TEST_BLOCK_SHIFT     12U                /* 4 KiB 블록 */

static u8 g_nyfs_test_ramdisk[NYFS_TEST_RAMDISK_SIZE] __attribute__((aligned(4096)));

static void test_nyfs(NTBLI *info)
{
    nyfs_mount_params_t params;
    Nstatus status;
    handle_t h;
    char readback[64];
    usize done;
    const char *payload = "hello from nyfs\n";
    u64 idx;
    char entry_name[64];
    u32 entry_type;

    status = ramdisk_init(NYFS_TEST_RAMDISK_DISKNO, (usize)g_nyfs_test_ramdisk,
                           NYFS_TEST_RAMDISK_SIZE, info);
    if (NSTATUS_IS_ERR(status)) {
        printk("nyfs test: ramdisk init failed: %r\n", status);
        return;
    }

    memset(&params, 0, sizeof(params));
    params.info = info;
    params.read = ramdisk_read;
    params.write = ramdisk_write;
    params.total_blocks = NYFS_TEST_RAMDISK_SIZE / (1U << NYFS_TEST_BLOCK_SHIFT);
    params.inode_count = 0;   /* 자동 */
    params.block_size_shift = (u8)NYFS_TEST_BLOCK_SHIFT;

    status = nyfs_format(NYFS_TEST_RAMDISK_DISKNO, &params);
    if (NSTATUS_IS_ERR(status)) {
        printk("nyfs test: format failed: %r\n", status);
        return;
    }

    params.mount_path = "nyfs:/";
    status = nyfs_mount(NYFS_TEST_RAMDISK_DISKNO, &params);
    if (NSTATUS_IS_ERR(status)) {
        printk("nyfs test: mount failed: %r\n", status);
        return;
    }

    status = vfs_mkdir("nyfs:/hello", 0755);
    if (NSTATUS_IS_ERR(status)) {
        printk("nyfs test: mkdir failed: %r\n", status);
        return;
    }

    status = vfs_create("nyfs:/hello/world.txt", 0644);
    if (NSTATUS_IS_ERR(status)) {
        printk("nyfs test: create failed: %r\n", status);
        return;
    }

    status = vfs_open("nyfs:/hello/world.txt", NX_O_WRITE, &h);
    if (NSTATUS_IS_ERR(status)) {
        printk("nyfs test: open(write) failed: %r\n", status);
        return;
    }
    status = vfs_write(&h, payload, strlen(payload), &done);
    (void)vfs_close(&h);
    if (NSTATUS_IS_ERR(status) || done != strlen(payload)) {
        printk("nyfs test: write failed: %r\n", status);
        return;
    }

    status = vfs_open("nyfs:/hello/world.txt", NX_O_READ, &h);
    if (NSTATUS_IS_ERR(status)) {
        printk("nyfs test: open(read) failed: %r\n", status);
        return;
    }
    status = vfs_read(&h, readback, sizeof(readback) - 1U, &done);
    (void)vfs_close(&h);
    if (NSTATUS_IS_ERR(status)) {
        printk("nyfs test: read failed: %r\n", status);
        return;
    }
    readback[done] = '\0';
    printk("nyfs test: read back %lu bytes: %s", (unsigned long)done, readback);

    printk("nyfs test: nyfs:/hello listing:\n");
    idx = 0;
    for (;;) {
        status = vfs_readdir("nyfs:/hello", idx, entry_name, sizeof(entry_name), &entry_type);
        if (status == NnotFound) {
            break;
        }
        if (NSTATUS_IS_ERR(status)) {
            printk("nyfs test: readdir failed: %r\n", status);
            break;
        }
        printk("  - %s (type=%u)\n", entry_name, (unsigned int)entry_type);
        idx++;
    }

    /* rename: 같은 디렉터리 안에서 이름 바꾸기 */
    status = vfs_rename("nyfs:/hello/world.txt", "nyfs:/hello/renamed.txt");
    if (NSTATUS_IS_ERR(status)) {
        printk("nyfs test: rename(same dir) failed: %r\n", status);
        return;
    }

    /* rename: 다른 디렉터리로 옮기기 */
    status = vfs_mkdir("nyfs:/moved", 0755);
    if (NSTATUS_IS_ERR(status)) {
        printk("nyfs test: mkdir(moved) failed: %r\n", status);
        return;
    }
    status = vfs_rename("nyfs:/hello/renamed.txt", "nyfs:/moved/final.txt");
    if (NSTATUS_IS_ERR(status)) {
        printk("nyfs test: rename(cross dir) failed: %r\n", status);
        return;
    }

    status = vfs_open("nyfs:/moved/final.txt", NX_O_READ, &h);
    if (NSTATUS_IS_ERR(status)) {
        printk("nyfs test: open after rename failed: %r\n", status);
        return;
    }
    status = vfs_read(&h, readback, sizeof(readback) - 1U, &done);
    (void)vfs_close(&h);
    if (NSTATUS_IS_ERR(status)) {
        printk("nyfs test: read after rename failed: %r\n", status);
        return;
    }
    readback[done] = '\0';
    printk("nyfs test: after rename, nyfs:/moved/final.txt = %s", readback);

    status = vfs_unlink("nyfs:/moved/final.txt");
    if (NSTATUS_IS_ERR(status)) {
        printk("nyfs test: unlink failed: %r\n", status);
        return;
    }

    printk("nyfs test: all steps completed\n");
}

/*
 * ring3 로 진입한다. iretq 프레임: ss, rsp, rflags, cs, rip 순서로 push.
 * 주의: 유저 주소 공간이 준비되지 않은 현재는 사용할 수 없다 (미검증).
 */
void enter_ring3(void *entry, void *stack_top)
{
    u64 user_cs = SEL_USER_CODE;
    u64 user_ss = SEL_USER_DATA;
    u64 user_rsp = (u64)(usize)stack_top;
    u64 user_rip = (u64)(usize)entry;
    u64 user_rflags = 0x202;    /* IF=1, IOPL=0 */

    __asm__ volatile(
        "pushq %0\n\t"
        "pushq %1\n\t"
        "pushq %2\n\t"
        "pushq %3\n\t"
        "pushq %4\n\t"
        "iretq"
        :
        : "r"(user_ss), "r"(user_rsp), "r"(user_rflags), "r"(user_cs), "r"(user_rip)
        : "memory"
    );

    for (;;)
        hlt();
}

#ifdef NYX_SELFTEST
extern void nyx_selftest(void);
#endif

/* Kernel main function - called from boot.s (higher-half) */
void kernel_main(u32 boot_kind, u64 boot_info_phys)
{
    Nstatus status;

    cli();

    /* 1. 로그 출력 수단부터 확보 (시리얼은 실패해도 계속 진행) */
    (void)serial_init();

    /* 2. 부트 정보를 NTBLI 로 통일 (UEFI 는 NTBLI 를 검증/복사, Multiboot2 는 변환) */
    if (boot_kind == BOOT_KIND_UEFI) {
        status = boot_info_from_uefi((const NTBLI *)(usize)boot_info_phys);
    } else if (boot_kind == BOOT_KIND_MULTIBOOT2) {
        status = multiboot2_to_ntbli(boot_info_phys, &g_boot_info, g_memmap_store, sizeof(g_memmap_store));
    } else {
        status = Nunsupported;
    }
    if (NSTATUS_IS_ERR(status)) {
        printk("Invalid boot info (kind %u): %r\n", boot_kind, status);
        kernel_panic_simple("Invalid boot info", status);
    }
    boot_info_sanitize_initrd();
    g_boot_info_valid = true;

    /* 3. CPU 구조: GDT/TSS, IDT, PIC, syscall (인터럽트는 아직 꺼져 있음) */
    gdt_init();
    gdt_set_kernel_stack((u64)(usize)boot_stack_top);
    interrupt_init();
    pic_remap(VEC_IRQ_BASE, VEC_IRQ_BASE + 8);
    status = syscall_init();
    if (NSTATUS_IS_ERR(status))
        printk("syscall/sysret unavailable (%r): int 0x80 only\n", status);

    /* 4. 메모리 보호: 자체 페이지 테이블(HHDM + higher-half 커널, W^X). 이후 낮은 주소 항등 매핑은 사라진다 */
    status = paging_init(&g_boot_info);
    if (NSTATUS_IS_ERR(status))
        fatal(status, "Paging init failed");
    paging_enable();
    boot_info_relocate();

    /* 5. 콘솔 (프레임버퍼는 HHDM 주소로 접근) */
    console_init_from_boot_info(&g_boot_info);

    printk("Nyxis OS Kernel Started (%s boot)\n", boot_kind == BOOT_KIND_UEFI ? "UEFI" : "Multiboot2");
    printk("Memory top: 0x%lx, initrd: %lu bytes, %ux%u framebuffer\n",
           (unsigned long)g_boot_info.memory_size,
           (unsigned long)g_boot_info.initrd_size,
           g_boot_info.width, g_boot_info.height);
    printk("GDT/IDT/PIC ready, paging enabled (higher-half, HHDM, W^X, NX, WP)\n");

    /* 6. 프로세스 / 타이머 */
    status = process_init();
    if (NSTATUS_IS_ERR(status))
        fatal(status, "Process init failed");

    status = timer_init(TIMER_HZ);
    if (NSTATUS_IS_ERR(status))
        fatal(status, "Timer init failed");

    /* 7. 파일시스템 */
    status = vfs_init();
    if (NSTATUS_IS_ERR(status))
        fatal(status, "Failed to initialize VFS");

    status = fat16_register();
    if (NSTATUS_IS_ERR(status))
        fatal(status, "Failed to register FAT16 file system");

    status = nyfs_register();
    if (NSTATUS_IS_ERR(status))
        fatal(status, "Failed to register nyfs file system");

    if (g_boot_info.initrd_base && g_boot_info.initrd_size) {
        mount_initrd(&g_boot_info);
        probe_first_app();
    } else {
        printk("No initrd loaded\n");
    }

    test_nyfs(&g_boot_info);

    /* 8. 저장장치 (없어도 계속 부팅) */
    status = ahci_init();
    if (NSTATUS_IS_ERR(status))
        printk("AHCI unavailable (%r): continuing without SATA storage\n", status);

    /* 9. 이제 핸들러가 모두 준비되었으므로 인터럽트를 켠다 */
    sti();

    printk("Kernel initialization complete. Userland loader is not implemented yet.\n");

#ifdef NYX_SELFTEST
    nyx_selftest();
#endif

    /* idle 루프: 실행 가능한 다른 프로세스가 있으면 양보하고, 없으면 인터럽트를 기다린다 */
    for (;;) {
        schedule();
        hlt();
    }
}
