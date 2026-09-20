/*
 * interrupt.c - IDT 구성, 인터럽트/예외 디스패치, 예외 진단 출력
 *
 * [수정 이력 요약]
 *  - 기존에는 IDT 가 kernel.c 안의 static 배열이었고 int 0x80 한 개만 채워져 있었다.
 *    나머지 255개 벡터가 비어 있어서 (P=0) 어떤 예외/IRQ 든 #GP -> #DF -> 트리플 폴트
 *    (= 조용한 리셋/정지) 로 이어졌다. 이제 256개 벡터를 모두 스텁으로 채운다.
 *  - find_current_status()/zero_div() 삭제:
 *      "현재 함수의 rsp 가 가리키는 메모리를 인터럽트 프레임이라고 간주" 하는 로직이라
 *      잘못된 메모리를 해석했다. 이제 스텁이 만든 정확한 trap_frame 을 받는다.
 *  - 커널 모드 예외는 레지스터/CR2 를 출력하고 패닉, 유저 모드 예외는 해당 프로세스만 종료.
 *  - 헤더에 static 변수를 정의하던 문제(현재 스택 포인터 등 미사용 변수) 제거.
 */

#include "nyxis.h"
#include "interrupt.h"
#include "console/outputs/printk.h"
#include "kernel/error_handling/panic.h"
#include "kernel/process/process.h"
#include "kernel/syscall/syscall.h"
#include "drivers/pic/pic.h"

/* isr_stubs.s 가 제공하는 256개 스텁 주소 테이블 */
extern const u64 isr_stub_table[VEC_COUNT];

static struct idt_entry g_idt[VEC_COUNT] __attribute__((aligned(16)));
static struct idtr g_idtr;

/* 벡터별 등록 핸들러 */
static interrupt_handler_t g_handlers[VEC_COUNT];

/* ------------------------------------------------------------------ */
/* IDT 저수준                                                          */
/* ------------------------------------------------------------------ */

void idt_set_gate(
    i32 vec,
    void *handler,
    struct idt_entry *idt,
    u8 flags,
    u8 ist
) {
    u64 handler_addr = (u64)(usize)handler;

    /* 잘못된 벡터 번호로 배열 밖을 쓰지 않도록 방어 */
    if (vec < 0 || vec >= (i32)VEC_COUNT || !idt)
        return;

    idt[vec].offset_low  = (u16)(handler_addr & 0xFFFFU);
    idt[vec].selector    = SEL_KERNEL_CODE;
    idt[vec].ist         = (u8)(ist & 0x7U);
    idt[vec].type_attr   = flags;
    idt[vec].offset_mid  = (u16)((handler_addr >> 16) & 0xFFFFU);
    idt[vec].offset_high = (u32)((handler_addr >> 32) & 0xFFFFFFFFUL);
    idt[vec].zero64      = 0;
}

void idt_init(u16 limit, u64 base)
{
    g_idtr.limit = limit;
    g_idtr.base = base;

    __asm__ volatile ("lidt %0" : : "m"(g_idtr) : "memory");
}

void interrupt_init(void)
{
    u32 v;

    for (v = 0; v < VEC_COUNT; v++) {
        u8 flags = IDT_INTERRUPT_GATE_KERNEL;
        u8 ist = 0;

        /* int 0x80 은 유저 모드(ring3)에서 호출 가능해야 하므로 DPL=3 */
        if (v == VEC_SYSCALL)
            flags = IDT_INTERRUPT_GATE_USER;

        /* 스택이 손상되었을 수 있는 치명적 예외는 전용 IST 스택 사용 */
        if (v == VEC_DOUBLE_FAULT)
            ist = 1;
        else if (v == VEC_NMI)
            ist = 2;
        else if (v == VEC_MACHINE_CHECK)
            ist = 3;

        idt_set_gate((i32)v, (void *)(usize)isr_stub_table[v], g_idt, flags, ist);
        g_handlers[v] = (interrupt_handler_t)0;
    }

    idt_init((u16)(sizeof(g_idt) - 1), (u64)(usize)g_idt);
}

/* ------------------------------------------------------------------ */
/* 핸들러 등록                                                         */
/* ------------------------------------------------------------------ */

Nstatus interrupt_register_handler(u32 vector, interrupt_handler_t handler)
{
    if (vector >= VEC_COUNT)
        return NinvalidArg;

    g_handlers[vector] = handler;
    return NSTATUS_OK;
}

Nstatus irq_register_handler(u8 irq, interrupt_handler_t handler)
{
    if (irq >= VEC_IRQ_COUNT)
        return NinvalidArg;

    g_handlers[VEC_IRQ_BASE + irq] = handler;
    return NSTATUS_OK;
}

/* ------------------------------------------------------------------ */
/* 예외 진단                                                           */
/* ------------------------------------------------------------------ */

const char *exception_name(u64 vector)
{
    switch (vector) {
    case VEC_DIVIDE_ERROR:         return "#DE Divide Error";
    case VEC_DEBUG:                return "#DB Debug";
    case VEC_NMI:                  return "NMI";
    case VEC_BREAKPOINT:           return "#BP Breakpoint";
    case VEC_OVERFLOW:             return "#OF Overflow";
    case VEC_BOUND_RANGE:          return "#BR Bound Range Exceeded";
    case VEC_INVALID_OPCODE:       return "#UD Invalid Opcode";
    case VEC_DEVICE_NOT_AVAIL:     return "#NM Device Not Available";
    case VEC_DOUBLE_FAULT:         return "#DF Double Fault";
    case VEC_INVALID_TSS:          return "#TS Invalid TSS";
    case VEC_SEGMENT_NOT_PRESENT:  return "#NP Segment Not Present";
    case VEC_STACK_FAULT:          return "#SS Stack-Segment Fault";
    case VEC_GENERAL_PROTECTION:   return "#GP General Protection";
    case VEC_PAGE_FAULT:           return "#PF Page Fault";
    case VEC_X87_FP:               return "#MF x87 Floating-Point";
    case VEC_ALIGNMENT_CHECK:      return "#AC Alignment Check";
    case VEC_MACHINE_CHECK:        return "#MC Machine Check";
    case VEC_SIMD_FP:              return "#XM SIMD Floating-Point";
    default:                       return "Reserved/Unknown exception";
    }
}

static void dump_frame(const struct trap_frame *f)
{
    printk("vector=%lu (%s) error=0x%lx\n", (unsigned long)f->vector,
           exception_name(f->vector), (unsigned long)f->error_code);
    printk("RIP=%p CS=0x%lx RFLAGS=0x%lx\n", (void *)(usize)f->rip,
           (unsigned long)f->cs, (unsigned long)f->rflags);
    printk("RSP=%p SS=0x%lx CR2=%p CR3=%p\n", (void *)(usize)f->rsp,
           (unsigned long)f->ss, (void *)(usize)read_cr2(),
           (void *)(usize)read_cr3());
    printk("RAX=%016lx RBX=%016lx RCX=%016lx\n", (unsigned long)f->rax,
           (unsigned long)f->rbx, (unsigned long)f->rcx);
    printk("RDX=%016lx RSI=%016lx RDI=%016lx\n", (unsigned long)f->rdx,
           (unsigned long)f->rsi, (unsigned long)f->rdi);
    printk("RBP=%016lx R8 =%016lx R9 =%016lx\n", (unsigned long)f->rbp,
           (unsigned long)f->r8, (unsigned long)f->r9);
    printk("R10=%016lx R11=%016lx R12=%016lx\n", (unsigned long)f->r10,
           (unsigned long)f->r11, (unsigned long)f->r12);
    printk("R13=%016lx R14=%016lx R15=%016lx\n", (unsigned long)f->r13,
           (unsigned long)f->r14, (unsigned long)f->r15);

    if (f->vector == VEC_PAGE_FAULT) {
        u64 e = f->error_code;

        printk("#PF: %s, %s, %s%s\n",
               (e & 1UL) ? "protection violation" : "page not present",
               (e & 2UL) ? "write" : "read",
               (e & 4UL) ? "user-mode" : "supervisor-mode",
               (e & 16UL) ? ", instruction fetch" : "");
    }
}

/* 등록된 핸들러가 없는 예외의 기본 처리 */
static void exception_default(struct trap_frame *f)
{
    if ((f->cs & 3UL) == 3UL) {
        /* 유저 모드 프로세스의 오류: 커널은 살리고 해당 프로세스만 종료한다 */
        printk("\nUser-mode fault, terminating process:\n");
        dump_frame(f);
        process_exit();
        /* process_exit 는 반환하지 않는다 */
    }

    printk("\n*** KERNEL EXCEPTION ***\n");
    dump_frame(f);
    kernel_panic_simple("Unhandled CPU exception in kernel mode", NkernelFault);
}

/* ------------------------------------------------------------------ */
/* 공통 디스패처 (isr_common 에서 호출)                                 */
/* ------------------------------------------------------------------ */

void interrupt_dispatch(struct trap_frame *frame)
{
    u64 v = frame->vector;
    interrupt_handler_t handler;

    if (v >= VEC_COUNT) {
        /* 스텁이 만들 수 없는 값. 프레임이 손상된 것이므로 패닉 */
        kernel_panic_simple("Corrupted trap frame (bad vector)", NkernelFault);
    }

    /* 하드웨어 IRQ (PIC) */
    if (v >= VEC_IRQ_BASE && v < VEC_IRQ_BASE + VEC_IRQ_COUNT) {
        u8 irq = (u8)(v - VEC_IRQ_BASE);

        if (pic_is_spurious(irq)) {
            /* 스퓨리어스 IRQ15 는 마스터에게만 EOI 를 보낸다. IRQ7 은 EOI 불필요. */
            if (irq == 15)
                pic_send_eoi(2);
            return;
        }

        handler = g_handlers[v];
        if (handler)
            handler(frame);

        pic_send_eoi(irq);
        return;
    }

    /* 시스템 콜 */
    if (v == VEC_SYSCALL) {
        syscall_handle(frame);
        return;
    }

    handler = g_handlers[v];
    if (handler) {
        handler(frame);
        return;
    }

    if (v < 32) {
        exception_default(frame);
        return;
    }

    /* 등록되지 않은 벡터(예: APIC 스퓨리어스): 로그만 남기고 무시 */
    printk("Ignoring unexpected interrupt vector %lu\n", (unsigned long)v);
}
