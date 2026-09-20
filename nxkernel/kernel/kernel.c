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
#include "drivers/pic/pic.h"
#include "boot_info.h"
#include "interrupt.h"
#include "memory.h"
#include "nyxis.h"

/* 멀티코어 여부 (현재는 BSP 만 사용) */
bool multicore_enabled = false;

/* 커널 소유 부트 정보 사본 */
static NTBLI g_boot_info;
static bool g_boot_info_valid = false;

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

/* 부트로더가 넘긴 NTBLI 를 검증하고 커널 소유 메모리에 복사한다 */
static Nstatus boot_info_validate_and_copy(const NTBLI *in)
{
    u64 end;

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

    /* initrd 범위: 오버플로/메모리 밖이면 initrd 를 사용하지 않는다 */
    if (g_boot_info.initrd_base || g_boot_info.initrd_size) {
        end = (u64)(usize)g_boot_info.initrd_base + g_boot_info.initrd_size;

        if (!g_boot_info.initrd_base || g_boot_info.initrd_size == 0 ||
            end < (u64)(usize)g_boot_info.initrd_base ||
            (g_boot_info.memory_size && end > g_boot_info.memory_size)) {
            g_boot_info.initrd_base = nNULL;
            g_boot_info.initrd_size = 0;
        }
    }

    g_boot_info_valid = true;
    return NSTATUS_OK;
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

/* Kernel main function - called from assembly entry point */
void kernel_main(NTBLI *boot_info)
{
    Nstatus status;

    cli();

    /* 1. 로그 출력 수단부터 확보 (시리얼은 실패해도 계속 진행) */
    (void)serial_init();

    /* 2. 부트 정보 검증 */
    status = boot_info_validate_and_copy(boot_info);
    if (NSTATUS_IS_ERR(status)) {
        printk("Invalid boot info from bootloader: %r\n", status);
        kernel_panic_simple("Invalid boot info", status);
    }

    console_init_from_boot_info(&g_boot_info);

    printk("Nyxis OS Kernel Started\n");
    printk("Memory top: 0x%lx, initrd: %lu bytes, %ux%u framebuffer\n",
           (unsigned long)g_boot_info.memory_size,
           (unsigned long)g_boot_info.initrd_size,
           g_boot_info.width, g_boot_info.height);

    /* 3. CPU 구조: GDT/TSS, IDT, PIC (인터럽트는 아직 꺼져 있음) */
    gdt_init();
    interrupt_init();
    pic_remap(VEC_IRQ_BASE, VEC_IRQ_BASE + 8);
    printk("GDT/IDT/PIC ready\n");

    /* 4. 메모리 보호: 자체 페이지 테이블 + W^X */
    status = paging_init(&g_boot_info);
    if (NSTATUS_IS_ERR(status))
        fatal(status, "Paging init failed");
    paging_enable();
    printk("Paging enabled (W^X, NX, WP)\n");

    /* 5. 프로세스 / 타이머 */
    status = process_init();
    if (NSTATUS_IS_ERR(status))
        fatal(status, "Process init failed");

    status = timer_init(TIMER_HZ);
    if (NSTATUS_IS_ERR(status))
        fatal(status, "Timer init failed");

    /* 6. 파일시스템 */
    status = vfs_init();
    if (NSTATUS_IS_ERR(status))
        fatal(status, "Failed to initialize VFS");

    status = fat16_register();
    if (NSTATUS_IS_ERR(status))
        fatal(status, "Failed to register FAT16 file system");

    if (g_boot_info.initrd_base && g_boot_info.initrd_size) {
        mount_initrd(&g_boot_info);
        probe_first_app();
    } else {
        printk("No initrd loaded\n");
    }

    /* 7. 저장장치 (없어도 계속 부팅) */
    status = ahci_init();
    if (NSTATUS_IS_ERR(status))
        printk("AHCI unavailable (%r): continuing without SATA storage\n", status);

    /* 8. 이제 핸들러가 모두 준비되었으므로 인터럽트를 켠다 */
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
