#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include "include/nyxis.h"

// Process states
typedef enum {
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_TERMINATED
} process_state_t;

typedef struct process {
    u32 pid;

    u32 state;

    void* stack;
    void* entry_point;

    void* cr3;

#ifdef NYXIS_64BITS

    u64 rax;
    u64 rbx;
    u64 rcx;
    u64 rdx;

    u64 rsi;
    u64 rdi;
    u64 rbp;

    u64 rsp;
    u64 rip;

    u64 rflags;

    u64 r8;
    u64 r9;
    u64 r10;
    u64 r11;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;

#else

    u32 eax;
    u32 ebx;
    u32 ecx;
    u32 edx;

    u32 esi;
    u32 edi;
    u32 ebp;

    u32 esp;
    u32 eip;

    u32 eflags;

#endif

    struct process* next;

} process_t;

// Functions
Nstatus process_init(void*);
Nstatus process_create(void* entry_point, void* stack);
Nstatus process_switch(process_t* proc);
Nstatus process_terminate(u32 pid);

#endif // KERNEL_PROCESS_H
