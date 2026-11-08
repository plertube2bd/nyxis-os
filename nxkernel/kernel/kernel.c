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
#include "kernel/mm/pfa.h"
#include "console/inputs/tty.h"
#include "kernel/timer/pit/pit_base.h"
#include "kernel/error_handling/panic.h"
#include "console/outputs/printk.h"
#include "console/outputs/conout.h"
#include "console/outputs/outputs_base.h"
#include "console/outputs/serial.h"
#include "drivers/ahci/ahci.h"
#include "drivers/ramdisk/ramdisk.h"
#include "drivers/filesystem/vfs.h"
#include "drivers/filesystem/fat16/fat16.h"
#include "drivers/filesystem/nyfs/nyfs.h"
#include "drivers/filesystem/nyfs/nyfs_disk.h"
#include "kernel/process/spawn.h"
#include "kernel/security/auth.h"
#include "kernel/dev/device.h"
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
extern u8 __boot_start[];
extern u8 __kernel_end[];

#define TIMER_HZ 100U

const NTBLI *get_kernel_info(void)
{
    return g_boot_info_valid ? &g_boot_info : (const NTBLI *)0;
}

/* 치명적 오류: 메시지를 출력하고 정지한다 */
static void fatal(Nstatus error, const char *message)
{
    printk_err("Kernel error: %s : %r\n", message, error);
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
        printk_warn("No framebuffer: using serial console only\n");
        return;
    }

    if (info->pixel_format != NTBLI_PIXEL_RGB && info->pixel_format != NTBLI_PIXEL_BGR) {
        printk_warn("Unsupported pixel format %u: framebuffer console disabled\n", info->pixel_format);
        return;
    }

    if (((usize)info->framebuffer_base & 3UL) != 0) {
        printk_warn("Misaligned framebuffer: framebuffer console disabled\n");
        return;
    }

    /* stride*height*4 바이트가 실제 프레임버퍼 크기 안에 있는지 확인 */
    needed = (u64)info->pixels_per_scan_line * (u64)info->height * 4UL;
    if (info->framebuffer_size < needed) {
        printk_warn("Framebuffer size mismatch: framebuffer console disabled\n");
        return;
    }

    status = conout_init((u32 *)info->framebuffer_base,
                         info->pixels_per_scan_line,
                         info->width,
                         info->height,
                         info->pixel_format);
    if (NSTATUS_IS_ERR(status)) {
        printk_warn("Framebuffer console init failed (%r): serial only\n", status);
        return;
    }

    if (font_is_ttf())
        printk_ok("Console font: TrueType %ux%u\n", font_cell_width(), font_cell_height());
    else if (NSTATUS_IS_ERR(font_init()))
        printk_warn("Console font: TrueType init failed (%r), using %ux%u bitmap\n",
               font_init(), font_cell_width(), font_cell_height());
    else
        printk_ok("Console font: %ux%u bitmap\n", font_cell_width(), font_cell_height());
}

/*
 * 루트 파일시스템: initrd 는 빌드 때 만든 nyfs 이미지다 (tools/nyfs-utils 의 nyfs-pack).
 * 부트로더가 RAM 에 올려 둔 그 이미지를 그대로 RAM 디스크(diskno 0)로 삼아 fs:/ 로 마운트한다.
 * 쓰기도 되지만 RAM 이라 재부팅하면 처음 이미지로 돌아간다 (영속 저장소는 아직 없다).
 * 구조: fs:/usr (= app:/), fs:/home/<사용자> (= face:/), fs:/roi (= roi:/, 사용자 DB 포함).
 */
#define ROOT_DISKNO        0U
#define TMP_DISKNO         1U
#define TMP_SIZE           (16U * 1024U * 1024U)  /* tmp:/ 16 MiB (컴파일 작업 공간으로도 쓴다) */
#define TMP_BLOCK_SHIFT    12U

/* tmp:/ 의 RAM 은 커널 이미지(BSS)에 두지 않는다: 부트로더가 커널을 고정 주소에 올릴 때 그만큼 연속된
 * 메모리가 필요해져 부팅이 실패할 수 있다. 대신 pfa 가 부팅 때 떼어 둔 연속 영역을 쓴다 (pfa_set_boot_reserve) */
static process_t *g_init = nNULL;

static bool mount_root(NTBLI *info)
{
    nyfs_mount_params_t params;
    Nstatus status;

    status = ramdisk_init(ROOT_DISKNO, (usize)info->initrd_base, (usize)info->initrd_size, info);
    if (NSTATUS_IS_ERR(status)) {
        printk_err("root: ramdisk init failed: %r\n", status);
        return false;
    }

    memset(&params, 0, sizeof(params));
    params.mount_path = "fs:/";
    params.info = info;
    params.read = ramdisk_read;
    params.write = ramdisk_write;

    status = nyfs_mount(ROOT_DISKNO, &params);
    if (NSTATUS_IS_ERR(status)) {
        printk_err("root: initrd is not a valid nyfs image (%r)\n", status);
        return false;
    }

    printk_ok("Root file system: fs:/ (nyfs, %lu KiB in RAM)\n", (unsigned long)(info->initrd_size / 1024UL));
    return true;
}

/* tmp:/ : 부팅할 때마다 새로 포맷하는 RAM nyfs (누구나 쓸 수 있음) */
static void mount_tmp(NTBLI *info)
{
    nyfs_mount_params_t params;
    Nstatus status;

    {
        u64 phys;

        status = pfa_boot_reserve(&phys);
        if (NSTATUS_IS_ERR(status)) {
            printk_warn("tmp: no contiguous memory for tmp:/ (%r)\n", status);
            return;
        }
        memset((void *)(usize)phys_to_virt(phys), 0, TMP_SIZE);
        status = ramdisk_init(TMP_DISKNO, (usize)phys_to_virt(phys), TMP_SIZE, info);
    }
    if (NSTATUS_IS_ERR(status)) {
        printk_warn("tmp: ramdisk init failed: %r\n", status);
        return;
    }

    memset(&params, 0, sizeof(params));
    params.info = info;
    params.read = ramdisk_read;
    params.write = ramdisk_write;
    params.total_blocks = TMP_SIZE / (1U << TMP_BLOCK_SHIFT);
    params.block_size_shift = (u8)TMP_BLOCK_SHIFT;
    params.root_mode = 0777;
    params.inode_count = 1024;           /* 자동값(블록 수/8)은 작은 파일이 많을 때 모자란다 */

    status = nyfs_format(TMP_DISKNO, &params);
    if (!NSTATUS_IS_ERR(status)) {
        params.mount_path = "tmp:/";
        status = nyfs_mount(TMP_DISKNO, &params);
    }
    if (NSTATUS_IS_ERR(status))
        printk_warn("tmp: format/mount failed: %r\n", status);
}

/*
 * disk:/ : 재부팅해도 남는 저장소. SATA 디스크 중 첫 섹터에 nyfs 슈퍼블록 매직이 있는 첫 디스크를 마운트한다.
 * 커널은 디스크를 절대 포맷하지 않는다 (실제 PC 에서 다른 OS 의 디스크를 지우지 않도록):
 * 디스크 이미지는 호스트에서 만든다 (nxkernel/Makefile 의 DISK_IMG, nyfs-pack).
 */
static void mount_disk(NTBLI *info)
{
    static u8 sector[512] __attribute__((aligned(16)));
    nyfs_mount_params_t params;
    u32 i;
    u32 count = ahci_disk_count();

    for (i = 0; i < count; i++) {
        u32 diskno = AHCI_DISKNO_BASE + i;
        u32 magic;
        Nstatus status = ahci_disk_read(diskno, 0, sector, sizeof(sector), info);

        if (NSTATUS_IS_ERR(status))
            continue;
        memcpy(&magic, sector, sizeof(magic));
        if (magic != NYFS_MAGIC)
            continue;

        memset(&params, 0, sizeof(params));
        params.mount_path = "disk:/";
        params.info = info;
        params.read = ahci_disk_read;
        params.write = ahci_disk_write;
        status = nyfs_mount(diskno, &params);
        if (NSTATUS_IS_ERR(status)) {
            printk_warn("disk: SATA disk %u has a bad nyfs volume (%r)\n", i, status);
            continue;
        }
        printk_ok("Persistent storage: disk:/ (nyfs on SATA disk %u)\n", i);
        return;
    }
    if (count > 0)
        printk_warn("disk: no nyfs-formatted SATA disk: disk:/ unavailable\n");
}

static void load_users(void)
{
    u32 n = 0;
    Nstatus status = auth_load_users("fs:/roi/users", &n);

    if (NSTATUS_IS_ERR(status))
        printk_err("auth: cannot read roi:/users (%r): nobody can log in\n", status);
    else
        printk_ok("auth: %u user(s) loaded from roi:/users\n", n);
}

/* 첫 사용자 프로세스 app:/init 을 root 로 띄운다 (자식들은 이 신원을 물려받는다) */
static void start_init(void)
{
    static const char init_args[] = "init";
    spawn_args_t args;
    Nstatus status;

    args.argc = 1;
    args.envc = 0;
    args.size = sizeof(init_args);
    args.strings = init_args;

    status = process_spawn("fs:/usr/init.run", &args, &g_init);
    if (NSTATUS_IS_ERR(status)) {
        printk_err("init: cannot start app:/init.run (%r)\n", status);
        g_init = nNULL;
        return;
    }
    printk_ok("init started (pid %u)\n", g_init->pid);
}

/*
 * ring3 로 진입한다. iretq 프레임: ss, rsp, rflags, cs, rip 순서로 push.
 * 호출 시점에 CR3 는 이미 현재 프로세스의 주소 공간이어야 한다 (process_switch 가 보장).
 */
void enter_ring3(void *entry, void *stack_top)
{
    u64 user_cs = SEL_USER_CODE;
    u64 user_ss = SEL_USER_DATA;
    u64 user_rsp = (u64)(usize)stack_top;
    u64 user_rip = (u64)(usize)entry;
    u64 user_rflags = 0x202;    /* IF=1, IOPL=0 */

    /*
     * iretq 직전에 범용 레지스터를 전부 0 으로 지운다. 지우지 않으면 커널 코드가 쓰던 값(커널 텍스트/
     * 스택/HHDM 주소 등)이 새 프로세스의 첫 명령 시점 레지스터에 그대로 남아 사용자에게 노출된다
     * (실제로 R8/R10/R12 에 0xffffffff80... 주소가 보였다 — 이후 KASLR 을 넣어도 무력화되는 정보 누출).
     * 피연산자 레지스터는 먼저 push 했으므로 지워도 된다. 이 함수는 반환하지 않으므로 clobber 도 상관없다.
     * 초기 스택 규약(umem.h)상 사용자 진입 시 레지스터 값에는 의미가 없다 (모두 0 이 기대값).
     */
    __asm__ volatile(
        "pushq %0\n\t"
        "pushq %1\n\t"
        "pushq %2\n\t"
        "pushq %3\n\t"
        "pushq %4\n\t"
        "xorl %%eax, %%eax\n\t"
        "xorl %%ebx, %%ebx\n\t"
        "xorl %%ecx, %%ecx\n\t"
        "xorl %%edx, %%edx\n\t"
        "xorl %%esi, %%esi\n\t"
        "xorl %%edi, %%edi\n\t"
        "xorl %%ebp, %%ebp\n\t"
        "xorl %%r8d, %%r8d\n\t"
        "xorl %%r9d, %%r9d\n\t"
        "xorl %%r10d, %%r10d\n\t"
        "xorl %%r11d, %%r11d\n\t"
        "xorl %%r12d, %%r12d\n\t"
        "xorl %%r13d, %%r13d\n\t"
        "xorl %%r14d, %%r14d\n\t"
        "xorl %%r15d, %%r15d\n\t"
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
        printk_err("Invalid boot info (kind %u): %r\n", boot_kind, status);
        kernel_panic_simple("Invalid boot info", status);
    }
    boot_info_sanitize_initrd();
    g_boot_info_valid = true;

    /* 3. CPU 구조: GDT/TSS, IDT, PIC, syscall (인터럽트는 아직 꺼져 있음) */
    gdt_init();
    gdt_set_kernel_stack((u64)(usize)boot_stack_top);
    cpu_fpu_init();
    interrupt_init();
    pic_remap(VEC_IRQ_BASE, VEC_IRQ_BASE + 8);
    status = syscall_init();
    if (NSTATUS_IS_ERR(status))
        printk_warn("syscall/sysret unavailable (%r): int 0x80 only\n", status);

    /* 4. 메모리 보호: 자체 페이지 테이블(HHDM + higher-half 커널, W^X). 이후 낮은 주소 항등 매핑은 사라진다 */
    status = paging_init(&g_boot_info);
    if (NSTATUS_IS_ERR(status))
        fatal(status, "Paging init failed");
    paging_enable();
    boot_info_relocate();

    /* 4.5. 물리 프레임 할당자: HHDM 이 막 켜졌으므로 이제부터 물리 페이지를 동적으로 내줄 수
     * 있다. 커널 이미지 자신과 initrd 의 물리 범위는 절대 자유 목록에 들어가면 안 된다 —
     * Multiboot2(E820 기반) 는 그 두 영역도 "사용 가능"으로 보고할 수 있으므로 메모리 타입과
     * 무관하게 항상 명시적으로 제외한다 (pfa.h 주석 참고). */
    {
        u64 kernel_phys_start = (u64)(usize)__boot_start;
        u64 kernel_phys_end = ((u64)(usize)__kernel_end - KERNEL_VMA_BASE + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);
        u64 initrd_phys_start = g_boot_info.initrd_base ? virt_to_phys(g_boot_info.initrd_base) : 0;
        u64 initrd_phys_end = g_boot_info.initrd_base ? initrd_phys_start + g_boot_info.initrd_size : 0;

        pfa_set_boot_reserve(TMP_SIZE);    /* tmp:/ 의 연속 RAM (mount_tmp) */
        status = pfa_init(&g_boot_info, kernel_phys_start, kernel_phys_end,
                          initrd_phys_start, initrd_phys_end, paging_hhdm_limit());
        if (NSTATUS_IS_ERR(status))
            fatal(status, "Physical frame allocator init failed");
    }

    /* 5. 콘솔 (프레임버퍼는 HHDM 주소로 접근) */
    console_init_from_boot_info(&g_boot_info);

    printk(KC_BOLD KC_CYAN "Nyxis OS Kernel Started" KC_RESET " (%s boot)\n", boot_kind == BOOT_KIND_UEFI ? "UEFI" : "Multiboot2");
    printk(KC_GRAY "Memory top: 0x%lx, initrd: %lu bytes, %ux%u framebuffer\n",
           (unsigned long)g_boot_info.memory_size,
           (unsigned long)g_boot_info.initrd_size,
           g_boot_info.width, g_boot_info.height);
    printk_ok("GDT/IDT/PIC ready, paging enabled (higher-half, HHDM, W^X, NX, WP)\n");
    printk_ok("Physical frame allocator ready: %lu free pages (%lu MiB)\n",
           (unsigned long)pfa_free_count(), (unsigned long)(pfa_free_count() * PAGE_SIZE / (1024UL * 1024UL)));

    /* 6. 프로세스 / 타이머 */
    status = process_init();
    if (NSTATUS_IS_ERR(status))
        fatal(status, "Process init failed");

    status = timer_init(TIMER_HZ);
    if (NSTATUS_IS_ERR(status))
        fatal(status, "Timer init failed");

    /* 콘솔 입력: PS/2 키보드(IRQ1) + 시리얼 수신(IRQ4) -> 줄 편집기 -> NxRead(stdin) */
    tty_init();

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

    {
        bool root_ok = false;

        if (g_boot_info.initrd_base && g_boot_info.initrd_size)
            root_ok = mount_root(&g_boot_info);
        else
            printk_warn("No initrd loaded: no root file system\n");

        mount_tmp(&g_boot_info);

        /* 장치: 프레임버퍼, COM1, 루트 RAM 디스크(마운트됨: 읽기만), rd0 */
        device_init(&g_boot_info, ROOT_DISKNO, root_ok ? g_boot_info.initrd_size : 0);

        if (root_ok)
            load_users();
    }

    /* 8. 저장장치 (없어도 계속 부팅) */
    status = ahci_init();
    if (NSTATUS_IS_ERR(status))
        printk_warn("AHCI unavailable (%r): continuing without SATA storage\n", status);
    else
        mount_disk(&g_boot_info);

    /* 9. 이제 핸들러가 모두 준비되었으므로 인터럽트를 켠다 */
    sti();

    printk_ok("Kernel initialization complete.\n");

#ifdef NYX_SELFTEST
    nyx_selftest();
#endif

    start_init();

    /* idle 루프: 실행 가능한 다른 프로세스가 있으면 양보하고, 없으면 인터럽트를 기다린다.
     * init 이 죽으면 로그인도 전원 끄기도 할 수 없으므로 멈춘다 (원인을 화면에 남긴다). */
    for (;;) {
        schedule();
        if (g_init && g_init->state == PROCESS_TERMINATED)
            kernel_panic_simple("init exited", NprocessFailed);
        hlt();
    }
}
