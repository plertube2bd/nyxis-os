/*
 * lowlevel.h - x86_64 저수준 프리미티브 (MSR/CPUID/포트 I/O/제어 레지스터 등)
 *
 * [수정 이력 요약]
 *  - C89 호환: 'static inline' -> 'static __inline__', '//' 주석 제거.
 *  - cli/sti/hlt 에 "memory" clobber 추가.
 *    (없으면 컴파일러가 임계구역 밖으로 메모리 접근을 재배치할 수 있음)
 *  - irq_save()/irq_restore() 추가: 인터럽트 플래그를 저장/복원한다.
 *    예전 spin_unlock 은 무조건 sti() 를 호출해서, 인터럽트가 꺼져 있어야 하는
 *    구간(부팅 초기, 중첩 락)에서 인터럽트를 잘못 켤 수 있었다.
 *  - cpuid: ECX(subleaf) 입력 추가.
 *
 * 주의: 이 헤더의 asm 은 스택 아래(레드존)를 사용하므로 커널은 반드시
 *       -mno-red-zone 으로 컴파일해야 한다.
 */
#ifndef LOWLEVEL_H
#define LOWLEVEL_H

#include "types.h"

/*
 * NYX_HOST_TEST: 호스트(리눅스 유저 공간)에서 파일시스템 파서 등을 테스트할 때
 * 특권 명령(cli/sti/hlt/in/out)을 사용하지 않도록 하는 스위치.
 * 실제 커널 빌드에서는 절대 정의하지 않는다.
 */

/* ============================ */
/* MSR (Model Specific Register) */
/* ============================ */

static __inline__ u64 rdmsr(u32 msr)
{
    u32 low, high;

    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));

    return ((u64)high << 32) | low;
}

static __inline__ void wrmsr(u32 msr, u64 value)
{
    u32 low  = (u32)(value & 0xFFFFFFFFUL);
    u32 high = (u32)(value >> 32);

    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high) : "memory");
}

/* ============================ */
/* CPUID                         */
/* ============================ */

static __inline__ void cpuid_count(u32 leaf, u32 subleaf,
                                   u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(subleaf));
}

static __inline__ void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
    cpuid_count(leaf, 0, a, b, c, d);
}

/* ============================ */
/* PORT IO                       */
/* ============================ */

static __inline__ u8 inb(u16 port)
{
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static __inline__ u16 inw(u16 port)
{
    u16 ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static __inline__ u32 inl(u16 port)
{
    u32 ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static __inline__ void outb(u16 port, u8 value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static __inline__ void outw(u16 port, u16 value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static __inline__ void outl(u16 port, u32 value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

/* 오래된 하드웨어용 짧은 지연 (미사용 포트 0x80 에 쓰기) */
static __inline__ void io_wait(void)
{
    __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}

/* ============================ */
/* FLAGS / INTERRUPT             */
/* ============================ */

#define RFLAGS_IF 0x200UL   /* Interrupt Enable Flag */

#ifdef NYX_HOST_TEST

static __inline__ void cli(void) { }
static __inline__ void sti(void) { }
static __inline__ void hlt(void) { }
static __inline__ u64 read_rflags(void) { return 0; }
static __inline__ u64 irq_save(void) { return 0; }
static __inline__ void irq_restore(u64 flags) { (void)flags; }

#else /* !NYX_HOST_TEST */

static __inline__ void cli(void)
{
    __asm__ volatile ("cli" : : : "memory");
}

static __inline__ void sti(void)
{
    __asm__ volatile ("sti" : : : "memory");
}

static __inline__ void hlt(void)
{
    __asm__ volatile ("hlt" : : : "memory");
}

/* ============================ */
/* RFLAGS                        */
/* ============================ */

static __inline__ u64 read_rflags(void)
{
    u64 flags;
    __asm__ volatile ("pushfq\n\t"
                      "popq %0"
                      : "=r"(flags)
                      :
                      : "memory");
    return flags;
}

/*
 * 현재 RFLAGS 를 반환하고 인터럽트 를 끈다.
 * 반드시 irq_restore(반환값) 와 짝을 맞춰 사용한다.
 */
static __inline__ u64 irq_save(void)
{
    u64 flags = read_rflags();
    cli();
    return flags;
}

/* irq_save() 가 반환한 값으로 인터럽트 상태를 복원한다 (원래 켜져 있었을 때만 sti) */
static __inline__ void irq_restore(u64 flags)
{
    if (flags & RFLAGS_IF)
        sti();
}

#endif /* NYX_HOST_TEST */

static __inline__ void write_rflags(u64 flags)
{
    __asm__ volatile ("pushq %0\n\t"
                      "popfq"
                      :
                      : "r"(flags)
                      : "memory", "cc");
}

/* ============================ */
/* CONTROL REGISTERS             */
/* ============================ */

static __inline__ u64 read_cr0(void)
{
    u64 val;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(val));
    return val;
}

static __inline__ void write_cr0(u64 val)
{
    __asm__ volatile ("mov %0, %%cr0" : : "r"(val) : "memory");
}

static __inline__ u64 read_cr2(void)
{
    u64 val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

static __inline__ u64 read_cr3(void)
{
    u64 val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static __inline__ void write_cr3(u64 val)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(val) : "memory");
}

static __inline__ u64 read_cr4(void)
{
    u64 val;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(val));
    return val;
}

static __inline__ void write_cr4(u64 val)
{
    __asm__ volatile ("mov %0, %%cr4" : : "r"(val) : "memory");
}

/* 단일 페이지의 TLB 엔트리 무효화 */
static __inline__ void invlpg(const void *addr)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

/* ============================ */
/* TSC (Time Stamp Counter)      */
/* ============================ */

static __inline__ u64 rdtsc(void)
{
    u32 low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((u64)high << 32) | low;
}

/* ============================ */
/* MEMORY BARRIER                */
/* ============================ */

static __inline__ void mfence(void)
{
    __asm__ volatile ("mfence" : : : "memory");
}

static __inline__ void lfence(void)
{
    __asm__ volatile ("lfence" : : : "memory");
}

static __inline__ void sfence(void)
{
    __asm__ volatile ("sfence" : : : "memory");
}

/* 컴파일러 배리어 (CPU 명령은 발행하지 않음) */
#define barrier() __asm__ volatile ("" : : : "memory")

/* ============================ */
/* PAUSE (spinlock 최적화)       */
/* ============================ */

static __inline__ void cpu_pause(void)
{
    __asm__ volatile ("pause");
}

#endif /* LOWLEVEL_H */
