#include "stdio.h"

void helloworld_main(void) {
    printk("hello, world. nyxis os!\n");
    while (1) {
        __asm__ volatile ("hlt");
    }
}
