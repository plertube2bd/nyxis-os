.section .multiboot2_header,"a",@progbits
    .align 8
    .long 0xE85250D6
    .long 0
    .long 24
    .long -(0xE85250D6 + 0 + 24)
    .long 0
    .long 8

.section .text
.global _start
.type _start, @function

/*
 * Kernel Entry Point (_start)
 * 
 * Called by bootloader after ExitBootServices() or Multiboot2.
 * 
 * Arguments:
 *   rdi: pointer to boot info structure (NTBLI or Multiboot2 info)
 */

_start:
    /* Set up stack */
    movq $0x1000000, %rsp        /* Stack pointer at high memory */
    sub $8, %rsp                /* Align stack to 16 bytes for ABI compliance */

    /* Convert boot info if required and call kernel_main */
    call boot_info_bridge
    movq %rax, %rdi
    call kernel_main

    /* Halt if kernel returns (shouldn't happen) */
    cli
_kernel_returned:
    hlt
    jmp _kernel_returned

.size _start, . - _start
