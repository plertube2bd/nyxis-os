#include "nyxis.h"
#include "lowlevel.h"
#include "kernel/paging/paging.h"
#include "console/outputs/printk.h"

__attribute__((noreturn))
void kernel_panic_simple(const char* Message, Nstatus error) {
    // disable_paging();
    printk("\n");
    printk("=========================================\n");
    printk("             KERNEL PANIC!\n");
    printk("=========================================\n");
    printk("System halted. should reboot.\n");

    for (;;) {
        cli();
        hlt();
    }
}
