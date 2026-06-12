#include "kernel/kernel.h"
#include "kernel/process/process.h"
#include "kernel/paging/paging.h"
#include "types.h"
#include "boot_info.h"
#include "lowlevel.h"
#include "console/outputs/printk.h"
#include "../../drivers/ahci/ahci.h"
#include "../../drivers/ramdisk/ramdisk.h"
#include "../../drivers/filesystem/vfs.h"
#include "../../drivers/filesystem/fat16/fat16.h"
#include "interrupt.h"
#include "kernel/error_handling/panic.h"

// Kernel information (NTBLI) - stored in kernel space
// Only accessible via get_kernel_info() function
static NTBLI* kernel_info = nNULL;
static NTBLI converted_boot_info;

bool multicore_enabled = false;

typedef struct {
    u32 total_size;
    u32 reserved;
} mb2_info_t;

typedef struct {
    u32 type;
    u32 size;
} mb2_tag_t;

typedef struct {
    mb2_tag_t tag;
    u32 mod_start;
    u32 mod_end;
    char cmdline[];
} mb2_module_tag_t;

typedef struct {
    mb2_tag_t tag;
    u32 mem_lower;
    u32 mem_upper;
} mb2_mem_tag_t;

typedef struct {
    mb2_tag_t tag;
    u64 framebuffer_addr;
    u32 framebuffer_pitch;
    u32 framebuffer_width;
    u32 framebuffer_height;
    u8 framebuffer_bpp;
    u8 framebuffer_type;
    u16 reserved;
} mb2_framebuffer_tag_t;

static bool is_multiboot2_info(void* info) {
    if (!info) {
        return false;
    }

    mb2_info_t* header = (mb2_info_t*)info;
    if (header->reserved != 0) {
        return false;
    }

    if (header->total_size < sizeof(mb2_info_t) || header->total_size > 0x1000000) {
        return false;
    }

    return true;
}

NTBLI* boot_info_bridge(NTBLI* boot_info) {
    if (!boot_info) {
        return nNULL;
    }

    if (!is_multiboot2_info((void*)boot_info)) {
        return boot_info;
    }

    mb2_info_t* info = (mb2_info_t*)boot_info;
    u8* cursor = (u8*)info + sizeof(mb2_info_t);
    u8* end = (u8*)info + info->total_size;

    memset(&converted_boot_info, 0, sizeof(converted_boot_info));
    converted_boot_info.version = 1;

    while (cursor < end) {
        mb2_tag_t* tag = (mb2_tag_t*)cursor;
        if (tag->size < sizeof(mb2_tag_t)) {
            break;
        }

        if (tag->type == 0) {
            break;
        }

        if (tag->type == 4) {
            mb2_mem_tag_t* mem = (mb2_mem_tag_t*)tag;
            converted_boot_info.memory_size = ((usize)mem->mem_lower + (usize)mem->mem_upper) * 1024;
        } else if (tag->type == 8) {
            mb2_framebuffer_tag_t* fb = (mb2_framebuffer_tag_t*)tag;
            converted_boot_info.framebuffer_base = (void*)(uintptr_t)fb->framebuffer_addr;
            converted_boot_info.framebuffer_size = (usize)fb->framebuffer_pitch * fb->framebuffer_height;
            converted_boot_info.width = fb->framebuffer_width;
            converted_boot_info.height = fb->framebuffer_height;
            converted_boot_info.pixels_per_scan_line = fb->framebuffer_bpp ? fb->framebuffer_pitch / (fb->framebuffer_bpp / 8) : 0;
        } else if (tag->type == 3) {
            mb2_module_tag_t* module = (mb2_module_tag_t*)tag;
            if (!converted_boot_info.initrd_base || !converted_boot_info.initrd_size) {
                converted_boot_info.initrd_base = (void*)(uintptr_t)module->mod_start;
                converted_boot_info.initrd_size = (usize)(module->mod_end - module->mod_start);
            }
        }

        cursor += ((tag->size + 7) & ~7);
    }

    return &converted_boot_info;
}

static struct idt_entry idt[256];
static u8 user_stack[8192] __attribute__((aligned(16)));

extern void helloworld_main(void);
extern void syscall_interrupt_handler(void);

static void enter_ring3(void* entry, void* stack_top) {
    u64 user_cs = 0x1B;
    u64 user_ss = 0x23;
    u64 user_rsp = (u64)stack_top;
    u64 user_rip = (u64)entry;
    u64 user_rflags = 0x202;

    __asm__ volatile(
        "pushq %0\n"
        "pushq %1\n"
        "pushq %2\n"
        "pushq %3\n"
        "pushq %4\n"
        "iretq\n"
        :
        : "r"(user_ss), "r"(user_rsp), "r"(user_rflags), "r"(user_cs), "r"(user_rip)
        : "memory"
    );
}

// Get kernel information - this is the only way to access NTBLI
// Only the kernel process or privileged code should call this
NTBLI* get_kernel_info() {
    return kernel_info;
}

Nstatus error_handler(Nstatus error, const char* message) {
    printk("Kernel error: %s : %x\n", message, error);
    kernel_panic_simple(message, error);
    return error;
}

// Process list
extern process_t* process_list;
extern u32 next_pid;

// Current process
extern process_t* current_process;

void zero_div(void) {
    struct status current_stack_status = find_current_status();
    if (current_stack_status.type) {
        process_terminate(current_process->pid);
    } else {
        error_handler(NdivideByZero, "Zero division error");
    }
}

// Kernel main function - called from assembly entry point
void kernel_main(NTBLI* boot_info) {
    kernel_info = boot_info;

    Nstatus status;

    status = vfs_init();
    if (NSTATUS_IS_ERR(status)) {
        error_handler(status, "Failed to initialize VFS");
    }

    status = fat16_register();
    if (NSTATUS_IS_ERR(status)) {
        error_handler(status, "Failed to register FAT16 file system");
    }

    if (boot_info->initrd_base && boot_info->initrd_size) {
        status = ramdisk_init(0, (usize)boot_info->initrd_base, boot_info->initrd_size, boot_info);
        if (NSTATUS_IS_ERR(status)) {
            error_handler(status, "Failed to initialize ramdisk");
        }

        fat16_mount_params_t initrd_params;
        initrd_params.mount_path = "initrd:/";
        initrd_params.info = boot_info;
        initrd_params.read = ramdisk_read;
        initrd_params.write = nNULL;

        status = fat16_mount(0, &initrd_params);
        if (NSTATUS_IS_ERR(status)) {
            error_handler(status, "Failed to mount initrd FAT16");
        }

        status = vfs_register_namespace("app", "initrd:/_ns_/app");
        if (NSTATUS_IS_ERR(status)) {
            status = vfs_register_namespace("app", "initrd:/");
            if (NSTATUS_IS_ERR(status)) {
                error_handler(status, "Failed to register app namespace");
            }
        }

        printk("Initrd available: %u bytes\n", (u32)boot_info->initrd_size);
    } else {
        printk("No initrd loaded\n");
    }

    status = printk_init(
        boot_info->framebuffer_base,
        boot_info->pixels_per_scan_line,
        boot_info->width,
        boot_info->height
    );
    if (NSTATUS_IS_ERR(status)) {
        error_handler(status, "Failed to initialize console output");
    }

    printk("Nyxis OS Kernel Started\n");
    printk("Resolution: %ux%u\n", boot_info->width, boot_info->height);

    if (boot_info->initrd_base && boot_info->initrd_size) {
        handle_t file_handle;
        usize read_bytes = 0;
        char buffer[256];

        status = vfs_open("app:/hellowld.run", 0, &file_handle);
        if (!NSTATUS_IS_ERR(status)) {
            status = vfs_read(&file_handle, buffer, sizeof(buffer) - 1, &read_bytes);
            if (!NSTATUS_IS_ERR(status)) {
                buffer[read_bytes] = '\0';
                printk("app:/hellowld.run:\n%s\n", buffer);
            }
            vfs_close(&file_handle);
        } else {
            printk("app:/hellowld.run not found: %x\n", status);
        }
    }

    status = paging_init();
    if (NSTATUS_IS_ERR(status)) {
        error_handler(status, "Paging init failed");
    }

    status = ahci_init();
    if (NSTATUS_IS_ERR(status)) {
        error_handler(status, "AHCI init failed");
    }

    idt_set_gate(0x80, syscall_interrupt_handler, idt, 0xEE, 0);
    idt_init(sizeof(idt) - 1, (u64)&idt);
    sti();

    printk("Entering ring 3 userland\n");

    enter_ring3(helloworld_main, &user_stack[sizeof(user_stack)]);

    while (1) {
        cpu_pause();
    }
}
