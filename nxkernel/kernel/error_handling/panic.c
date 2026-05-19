#pragma once

#include "include/nyxis.h"
#include "include/lowlevel.h"
#include "kernel/paging/paging.h"

__attribute__((noreturn))
void kernel_panic_simple(const char* Message, Nstatus error) {
    disable_paging();
    printk("\n");
    printk("=========================================\n");
    printk("             KERNEL PANIC!\n");
    printk("=========================================\n");

    if (Message) {
        printk("Reason: %s code: %ux(%r)\n", Message, error, error);
    }

    printk("System halted. should reboot.\n");

    for (;;) {
        cli();
        hlt();
    }
}
