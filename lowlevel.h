#ifndef LOWLEVEL_H
#define LOWLEVEL_H

#include "nyxis.h"

// ============================
// MSR (Model Specific Register)
// ============================

static inline u64 rdmsr(u32 msr) {
    u32 low, high;

    __asm__ volatile (
        "rdmsr"
        : "=a"(low), "=d"(high)
        : "c"(msr)
    );

    return ((u64)high << 32) | low;
}

static inline void wrmsr(u32 msr, u64 value) {
    u32 low  = (u32)(value & 0xFFFFFFFF);
    u32 high = (u32)(value >> 32);

    __asm__ volatile (
        "wrmsr"
        :
        : "c"(msr), "a"(low), "d"(high)
        : "memory"
    );
}


// ============================
// CPUID
// ============================

static inline void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile (
        "cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf)
    );
}


// ============================
// PORT IO
// ============================

static inline u8 inb(u16 port) {
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline u16 inw(u16 port) {
    u16 ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline u32 inl(u16 port) {
    u32 ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(u16 port, u16 value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outl(u16 port, u32 value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline void io_wait() {
    __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}


// ============================
// FLAGS / INTERRUPT
// ============================

static inline void cli() {
    __asm__ volatile ("cli");
}

static inline void sti() {
    __asm__ volatile ("sti");
}

static inline void hlt() {
    __asm__ volatile ("hlt");
}


// ============================
// RFLAGS
// ============================

static inline u64 read_rflags() {
    u64 flags;
    __asm__ volatile (
        "pushfq\n\t"
        "pop %0"
        : "=r"(flags)
    );
    return flags;
}

static inline void write_rflags(u64 flags) {
    __asm__ volatile (
        "push %0\n\t"
        "popfq"
        :
        : "r"(flags)
        : "memory"
    );
}


// ============================
// CONTROL REGISTERS
// ============================

static inline u64 read_cr0() {
    u64 val;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline void write_cr0(u64 val) {
    __asm__ volatile ("mov %0, %%cr0" : : "r"(val) : "memory");
}

static inline u64 read_cr3() {
    u64 val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(u64 val) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(val) : "memory");
}

static inline u64 read_cr4() {
    u64 val;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline void write_cr4(u64 val) {
    __asm__ volatile ("mov %0, %%cr4" : : "r"(val) : "memory");
}


// ============================
// TSC (Time Stamp Counter)
// ============================

static inline u64 rdtsc() {
    u32 low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((u64)high << 32) | low;
}


// ============================
// MEMORY BARRIER
// ============================

static inline void mfence() {
    __asm__ volatile ("mfence" ::: "memory");
}

static inline void lfence() {
    __asm__ volatile ("lfence" ::: "memory");
}

static inline void sfence() {
    __asm__ volatile ("sfence" ::: "memory");
}


// ============================
// PAUSE (spinlock 최적화)
// ============================

static inline void cpu_pause() {
    __asm__ volatile ("pause");
}

#endif