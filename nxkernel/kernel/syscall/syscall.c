#include "nyxis.h"
#include "console/outputs/printk.h"

Nstatus syscall_dispatch(
    u64 syscall_nr,
    u64 rdi,
    u64 rsi,
    u64 rdx,
    u64 r10,
    u64 r8,
    u64 r9
) {
    switch (syscall_nr) {
        case 771: {
            if (!rdi) {
                return NinvalidPointer;
            }
            const char* message = (const char*)(usize)rdi;
            printk(message);
            return NSTATUS_OK;
        }
        default:
            return NsyscallFailed;
    }
}

__attribute__((naked)) void syscall_interrupt_handler(void) {
    asm volatile(
        "pushq %%rbp\n"
        "pushq %%rbx\n"
        "pushq %%rcx\n"
        "pushq %%rdx\n"
        "pushq %%rsi\n"
        "pushq %%rdi\n"
        "pushq %%r8\n"
        "pushq %%r9\n"
        "pushq %%r10\n"
        "pushq %%r11\n"
        "pushq %%r12\n"
        "pushq %%r13\n"
        "pushq %%r14\n"
        "pushq %%r15\n"
        "movq %%rdi, %%r11\n"
        "movq %%rsi, %%r12\n"
        "movq %%rdx, %%r13\n"
        "movq %%rax, %%rdi\n"
        "movq %%r11, %%rsi\n"
        "movq %%r12, %%rdx\n"
        "movq %%r13, %%r10\n"
        "call syscall_dispatch\n"
        "popq %%r15\n"
        "popq %%r14\n"
        "popq %%r13\n"
        "popq %%r12\n"
        "popq %%r11\n"
        "popq %%r10\n"
        "popq %%r9\n"
        "popq %%r8\n"
        "popq %%rdi\n"
        "popq %%rsi\n"
        "popq %%rdx\n"
        "popq %%rcx\n"
        "popq %%rbx\n"
        "popq %%rbp\n"
        "iretq\n"
        :::"memory"
    );
}

