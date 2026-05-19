#ifndef NYXSTD_H
# define NYXSTD_H

# include <stdint.h>

# define __u64 unsigned long long
# define __u32 unsigned int

# if INTPTR_MAX == 9223372036854775807LL
#  define _NYX64

static inline __u64 syscall_wrapper(__u64 sysno, __u64 rdi, __u64 rsi, __u64 rdx, __u64 r10, __u64 r8, __u64 r9) {
    __u64 ret;
    __asm__ volatile (
        "movq %1, %%rax\n\t"
        "movq %2, %%rdi\n\t"
        "movq %3, %%rsi\n\t"
        "movq %4, %%rdx\n\t"
        "movq %5, %%r10\n\t"
        "movq %6, %%r8\n\t"
        "movq %7, %%r9\n\t"
        "syscall\n\t"
        : "=a"(ret)
        : "r"(sysno), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

# else

static inline __u32 syscall_wrapper(__u32 sysno, __u32 ebx, __u32 ecx, __u32 edx, __u32 esi, __u32 edi, __u32 ebp) {
    __u32 ret;
    __asm__ volatile (
        "movl %1, %%eax\n\t"
        "movl %2, %%ebx\n\t"
        "movl %3, %%ecx\n\t"
        "movl %4, %%edx\n\t"
        "movl %5, %%esi\n\t"
        "movl %6, %%edi\n\t"
        "movl %7, %%ebp\n\t"
        "syscall\n\t"
        : "=a"(ret)
        : "r"(sysno), "r"(ebx), "r"(ecx), "r"(edx), "r"(esi), "r"(edi), "r"(ebp)
        : "ecx", "memory"
    );
    return ret;
}

# endif
#endif