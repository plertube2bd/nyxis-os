#include "kernel/kernel.h"
#include "kernel/process/process.h"
#include "types.h"
#include "boot_info.h"
#include "lowlevel.h"
#include "console/outputs/printk.h"
#include "include/interrupt.h"
#include "kernel/error_handling/panic.h"

// Interrupt
struct idt_entry idt;

// Kernel information (NTBLI) - stored in kernel space
// Only accessible via get_kernel_info() function
static NTBLI* kernel_info = nNULL;

// Get kernel information - this is the only way to access NTBLI
// Only the kernel process or privileged code should call this
NTBLI* get_kernel_info() {
    // In a full implementation, add privilege checks here
    // For now, return the kernel info
    return kernel_info;
    // after multiprocessing, we can add process-specific info here as well
}

// Process list
extern process_t* process_list;
extern u32 next_pid;

// Current process
extern process_t* current_process;

void zero_div(void) {
	__asm__ volatile ("push $0");
	//hahaha
	struct status current_stack_status = find_current_status();
	if (current_stack_status.type) {
		process_terminate(current_process->pid);
	}
	else { kernel_panic_simple("Zero division error", NdivideByZero); }
}

// Kernel main function - called from assembly entry point
void kernel_main(NTBLI* boot_info) {
    // Store boot information in kernel space
    kernel_info = boot_info;

    // Initialize kernel subsystems
    Nstatus status;

    // Initialize process management
    // status = process_init();
    // if (NSTATUS_IS_ERR(status)) {
    //     hlt();
    // }

    // Initialize console output
    status = printk_init(
        boot_info->framebuffer_base,
        boot_info->pixels_per_scan_line,
        boot_info->width,
        boot_info->height
    );
    if (NSTATUS_IS_ERR(status)) {
        kernel_panic_simple("Failed to initialize console output", status);
    }

    // Print boot message
    printk("Nyxis OS Kernel Started\n");
    printk("Resolution: %ux%u\n", boot_info->width, boot_info->height);
    
    // Set up Interrupt Handers
    idt_set_gate(0, zero_div, &idt, 0x8E, 0x0);
    

    // TODO: Create first user process
    // TODO: Set up interrupt handlers
    // TODO: Initialize other subsystems

    // For now, just loop
    while (1) {
        hlt();
    }
}
