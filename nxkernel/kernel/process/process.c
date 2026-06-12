#include "kernel/process/process.h"
#include "kernel/paging/paging.h"

// Process list
process_t* process_list = 0;
u32 next_pid = 1;

// Current process
process_t* current_process = 0;

// Initialize process system
Nstatus process_init(void* plus) {
    // Create idle process
    process_t* idle = (process_t*)plus; // Example allocation
    idle->pid = 0;
    idle->state = PROCESS_RUNNING;
    idle->stack = (void*)0x300000;
    idle->entry_point = 0;
    idle->next = 0;

#ifdef NYXIS_64BITS
    idle->rsp = (u64)idle->stack;
    idle->rip = 0;
    idle->rflags = 0x202;
    idle->rax = idle->rbx = idle->rcx = idle->rdx = 0;
    idle->rsi = idle->rdi = idle->rbp = 0;
    idle->r8 = idle->r9 = idle->r10 = idle->r11 = 0;
    idle->r12 = idle->r13 = idle->r14 = idle->r15 = 0;
#else
    idle->esp = (u32)idle->stack;
    idle->eip = 0;
    idle->eflags = 0x202;
    idle->eax = idle->ebx = idle->ecx = idle->edx = 0;
    idle->esi = idle->edi = idle->ebp = 0;
#endif

    process_list = idle;
    current_process = idle;

    return NSTATUS_OK;
}

// Create a new process
Nstatus process_create(void* entry_point, void* stack) {
    process_t* proc = (process_t*)((u8*)process_list + sizeof(process_t) * next_pid);
    proc->pid = next_pid++;
    proc->state = PROCESS_READY;
    proc->stack = stack;
    proc->entry_point = entry_point;
    proc->next = process_list;
    process_list = proc;

#ifdef NYXIS_64BITS
    proc->rsp = (u64)stack;
    proc->rip = (u64)entry_point;
    proc->rflags = 0x202; // IF flag set
    proc->rax = proc->rbx = proc->rcx = proc->rdx = 0;
    proc->rsi = proc->rdi = proc->rbp = 0;
    proc->r8 = proc->r9 = proc->r10 = proc->r11 = 0;
    proc->r12 = proc->r13 = proc->r14 = proc->r15 = 0;
#else
    proc->esp = (u32)stack;
    proc->eip = (u32)entry_point;
    proc->eflags = 0x202; // IF flag set
    proc->eax = proc->ebx = proc->ecx = proc->edx = 0;
    proc->esi = proc->edi = proc->ebp = 0;
#endif

    return NSTATUS_OK;
}

// Switch to a process
Nstatus process_switch(process_t* proc) {
    if (!proc || proc->state != PROCESS_READY) {
        return NSTATUS_ERR_FLAG | 1; // Error
    }

    if (current_process && current_process->state == PROCESS_RUNNING) {
        current_process->state = PROCESS_READY;
    }

    // Save current context
    if (current_process) {
#ifdef NYXIS_64BITS
        u64 saved_rax, saved_rbx, saved_rcx, saved_rdx;
        u64 saved_rsi, saved_rdi, saved_rbp, saved_rsp;
        u64 saved_r8, saved_r9, saved_r10, saved_r11;
        u64 saved_r12, saved_r13, saved_r14, saved_r15;
        u64 saved_flags;

        __asm__ volatile("movq %%rax, %0" : "=r"(saved_rax) :: "memory");
        __asm__ volatile("movq %%rbx, %0" : "=r"(saved_rbx) :: "memory");
        __asm__ volatile("movq %%rcx, %0" : "=r"(saved_rcx) :: "memory");
        __asm__ volatile("movq %%rdx, %0" : "=r"(saved_rdx) :: "memory");
        __asm__ volatile("movq %%rsi, %0" : "=r"(saved_rsi) :: "memory");
        __asm__ volatile("movq %%rdi, %0" : "=r"(saved_rdi) :: "memory");
        __asm__ volatile("movq %%rbp, %0" : "=r"(saved_rbp) :: "memory");
        __asm__ volatile("movq %%rsp, %0" : "=r"(saved_rsp) :: "memory");
        __asm__ volatile("movq %%r8, %0"  : "=r"(saved_r8)  :: "memory");
        __asm__ volatile("movq %%r9, %0"  : "=r"(saved_r9)  :: "memory");
        __asm__ volatile("movq %%r10, %0" : "=r"(saved_r10) :: "memory");
        __asm__ volatile("movq %%r11, %0" : "=r"(saved_r11) :: "memory");
        __asm__ volatile("movq %%r12, %0" : "=r"(saved_r12) :: "memory");
        __asm__ volatile("movq %%r13, %0" : "=r"(saved_r13) :: "memory");
        __asm__ volatile("movq %%r14, %0" : "=r"(saved_r14) :: "memory");
        __asm__ volatile("movq %%r15, %0" : "=r"(saved_r15) :: "memory");
        __asm__ volatile("pushfq\n popq %0" : "=r"(saved_flags) :: "memory");

        current_process->rax = saved_rax;
        current_process->rbx = saved_rbx;
        current_process->rcx = saved_rcx;
        current_process->rdx = saved_rdx;
        current_process->rsi = saved_rsi;
        current_process->rdi = saved_rdi;
        current_process->rbp = saved_rbp;
        current_process->rsp = saved_rsp;
        current_process->r8  = saved_r8;
        current_process->r9  = saved_r9;
        current_process->r10 = saved_r10;
        current_process->r11 = saved_r11;
        current_process->r12 = saved_r12;
        current_process->r13 = saved_r13;
        current_process->r14 = saved_r14;
        current_process->r15 = saved_r15;
        current_process->rflags = saved_flags;
#else
        u32 saved_flags;
        __asm__ volatile("mov %%eax, %0"    : "=r"(current_process->eax) :: "memory");
        __asm__ volatile("mov %%ebx, %0"    : "=r"(current_process->ebx) :: "memory");
        __asm__ volatile("mov %%ecx, %0"    : "=r"(current_process->ecx) :: "memory");
        __asm__ volatile("mov %%edx, %0"    : "=r"(current_process->edx) :: "memory");
        __asm__ volatile("mov %%esi, %0"    : "=r"(current_process->esi) :: "memory");
        __asm__ volatile("mov %%edi, %0"    : "=r"(current_process->edi) :: "memory");
        __asm__ volatile("mov %%ebp, %0"    : "=r"(current_process->ebp) :: "memory");
        __asm__ volatile("mov %%esp, %0"    : "=r"(current_process->esp) :: "memory");
        __asm__ volatile("pushfl\n popl %0" : "=r"(saved_flags) :: "memory");
        current_process->eflags = saved_flags;
#endif
    }

    // Switch page directory
    if (proc->cr3) {
        __asm__ volatile("mov %0, %%cr3" : : "r"(proc->cr3));
    }

    // Load new context
    current_process = proc;
    proc->state = PROCESS_RUNNING;

#ifdef NYXIS_64BITS
    __asm__ volatile("movq %0, %%rax" :: "m"(proc->rax) : "memory");
    __asm__ volatile("movq %0, %%rbx" :: "m"(proc->rbx) : "memory");
    __asm__ volatile("movq %0, %%rcx" :: "m"(proc->rcx) : "memory");
    __asm__ volatile("movq %0, %%rdx" :: "m"(proc->rdx) : "memory");
    __asm__ volatile("movq %0, %%rsi" :: "m"(proc->rsi) : "memory");
    __asm__ volatile("movq %0, %%rdi" :: "m"(proc->rdi) : "memory");
    __asm__ volatile("movq %0, %%rbp" :: "m"(proc->rbp) : "memory");
    __asm__ volatile("movq %0, %%rsp" :: "m"(proc->rsp) : "memory");
    __asm__ volatile("movq %0, %%r8"  :: "m"(proc->r8)  : "memory");
    __asm__ volatile("movq %0, %%r9"  :: "m"(proc->r9)  : "memory");
    __asm__ volatile("movq %0, %%r10" :: "m"(proc->r10) : "memory");
    __asm__ volatile("movq %0, %%r11" :: "m"(proc->r11) : "memory");
    __asm__ volatile("movq %0, %%r12" :: "m"(proc->r12) : "memory");
    __asm__ volatile("movq %0, %%r13" :: "m"(proc->r13) : "memory");
    __asm__ volatile("movq %0, %%r14" :: "m"(proc->r14) : "memory");
    __asm__ volatile("movq %0, %%r15" :: "m"(proc->r15) : "memory");
    __asm__ volatile("pushq %0\n popfq" :: "r"(proc->rflags) : "memory");
    __asm__ volatile("jmp *%0" :: "m"(proc->rip) : "memory");
#else
    __asm__ volatile("mov %0, %%eax" :: "m"(proc->eax) : "memory");
    __asm__ volatile("mov %0, %%ebx" :: "m"(proc->ebx) : "memory");
    __asm__ volatile("mov %0, %%ecx" :: "m"(proc->ecx) : "memory");
    __asm__ volatile("mov %0, %%edx" :: "m"(proc->edx) : "memory");
    __asm__ volatile("mov %0, %%esi" :: "m"(proc->esi) : "memory");
    __asm__ volatile("mov %0, %%edi" :: "m"(proc->edi) : "memory");
    __asm__ volatile("mov %0, %%ebp" :: "m"(proc->ebp) : "memory");
    __asm__ volatile("mov %0, %%esp" :: "m"(proc->esp) : "memory");
    __asm__ volatile("pushl %0\n popfl" :: "r"(proc->eflags) : "memory");
    __asm__ volatile("jmp *%0" :: "m"(proc->eip) : "memory");
#endif

    return NSTATUS_OK;
}

// Terminate a process
Nstatus process_terminate(u32 pid) {
    process_t* proc = process_list;
    while (proc) {
        if (proc->pid == pid) {
            proc->state = PROCESS_TERMINATED;
            // Remove from list, free memory, etc.
            return NSTATUS_OK;
        }
        proc = proc->next;
    }
    return NSTATUS_ERR_FLAG | 2; // Not found
}
