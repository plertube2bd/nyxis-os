/*
 * interrupt.h - x86_64 GDT / TSS / IDT / 트랩 프레임 정의 (커널 공용)
 *
 * [수정 이력 요약]
 *  - 이 파일은 "32비트용" 이라고 적혀 있었지만 실제로는 64비트 IDT 를 정의하고
 *    있었고, 이후 코드는 64비트 전용이므로 32비트 잔재(frame32 등)를 제거했다.
 *  - 실제 인터럽트 진입 스텁이 만드는 스택 레이아웃과 일치하는 struct trap_frame
 *    을 정의했다. (기존 interrupt_error_frame64_* 는 실제 스택과 맞지 않았고
 *    ring 이 같은 경우에도 rsp/ss 를 포함하고 있었다)
 *  - 세그먼트 셀렉터 상수를 정의했다. (기존 코드는 IDT 게이트 셀렉터를 0x08 로
 *    하드코딩했지만 UEFI 의 GDT 에서 0x08 은 64비트 코드 세그먼트가 아니었다)
 *  - 깨진 zero_div()/find_current_status() 선언을 제거했다.
 */
#ifndef NYXIS_INTERRUPT_H
#define NYXIS_INTERRUPT_H

#include "nyxis.h"

/* ------------------------------------------------------------------ */
/* GDT 셀렉터 (kernel/interrupt/gdt.c 의 GDT 배열 순서와 반드시 일치)    */
/* ------------------------------------------------------------------ */
#define SEL_KERNEL_CODE  0x08U   /* ring0 64비트 코드 */
#define SEL_KERNEL_DATA  0x10U   /* ring0 데이터 */
#define SEL_USER_CODE32  0x1BU   /* ring3 32비트 코드 (호환 모드용, RPL=3 포함) */
#define SEL_USER_DATA    0x23U   /* ring3 데이터      (RPL=3 포함) */
#define SEL_USER_CODE    0x2BU   /* ring3 64비트 코드 (RPL=3 포함) */
#define SEL_TSS          0x30U   /* TSS (16바이트 디스크립터) */

/*
 * SYSCALL/SYSRET 규칙에 맞춘 배치:
 *   STAR[47:32] = 0x08 -> SYSCALL: CS = 0x08,      SS = 0x10
 *   STAR[63:48] = 0x18 -> SYSRET64: SS = 0x18+8|3 = 0x23 (유저 데이터), CS = 0x18+16|3 = 0x2B (유저 코드64)
 *                         SYSRET32: CS = 0x18|3   = 0x1B (유저 코드32)
 * 그래서 GDT 는 "유저 코드32 -> 유저 데이터 -> 유저 코드64" 순서여야 한다.
 */
#define STAR_KERNEL_BASE 0x08U
#define STAR_USER_BASE   0x18U

/* ------------------------------------------------------------------ */
/* 벡터 번호                                                           */
/* ------------------------------------------------------------------ */
#define VEC_DIVIDE_ERROR        0U
#define VEC_DEBUG               1U
#define VEC_NMI                 2U
#define VEC_BREAKPOINT          3U
#define VEC_OVERFLOW            4U
#define VEC_BOUND_RANGE         5U
#define VEC_INVALID_OPCODE      6U
#define VEC_DEVICE_NOT_AVAIL    7U
#define VEC_DOUBLE_FAULT        8U
#define VEC_INVALID_TSS        10U
#define VEC_SEGMENT_NOT_PRESENT 11U
#define VEC_STACK_FAULT        12U
#define VEC_GENERAL_PROTECTION 13U
#define VEC_PAGE_FAULT         14U
#define VEC_X87_FP             16U
#define VEC_ALIGNMENT_CHECK    17U
#define VEC_MACHINE_CHECK      18U
#define VEC_SIMD_FP            19U
#define VEC_IRQ_BASE           0x20U   /* PIC IRQ0 -> 벡터 0x20 */
#define VEC_IRQ_COUNT          16U
#define VEC_SYSCALL            0x80U   /* int 0x80 시스템 콜 */
#define VEC_COUNT              256U

/* ------------------------------------------------------------------ */
/* IDT / GDT 레지스터 및 엔트리                                        */
/* ------------------------------------------------------------------ */
struct idtr {
    u16 limit;
    u64 base;
} pack;

struct idt_entry {
    u16 offset_low;
    u16 selector;
    u8  ist;         /* 하위 3비트만 IST 인덱스 (0 = 사용 안 함) */
    u8  type_attr;   /* P(7) DPL(6:5) 0(4) Type(3:0) */
    u16 offset_mid;
    u32 offset_high;
    u32 zero64;
} pack;

NX_STATIC_ASSERT(idt_entry_size, sizeof(struct idt_entry) == 16);
NX_STATIC_ASSERT(idtr_size, sizeof(struct idtr) == 10);

/* type_attr 값 */
#define IDT_INTERRUPT_GATE_KERNEL 0x8EU  /* P=1 DPL=0 64비트 인터럽트 게이트 */
#define IDT_INTERRUPT_GATE_USER   0xEEU  /* P=1 DPL=3 64비트 인터럽트 게이트 */
#define IDT_TRAP_GATE_KERNEL      0x8FU  /* P=1 DPL=0 64비트 트랩 게이트 */

/* 64비트 TSS (규격상 104바이트) */
struct tss64 {
    u32 res1;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 res2;
    u64 ist1;
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;
    u64 res3;
    u16 res4;
    u16 iomap_base;
} pack;

NX_STATIC_ASSERT(tss64_size, sizeof(struct tss64) == 104);

/* ------------------------------------------------------------------ */
/* 트랩 프레임                                                         */
/*                                                                     */
/* isr_stubs.s 가 만드는 스택 (낮은 주소 -> 높은 주소):                */
/*   r15 ... rax (소프트웨어가 push), vector, error_code (스텁이 push),*/
/*   rip, cs, rflags, rsp, ss (CPU 가 push)                            */
/* 64비트 모드에서는 ring 전환이 없어도 rsp/ss 가 항상 push 된다.       */
/* ------------------------------------------------------------------ */
struct trap_frame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector;
    u64 error_code;   /* CPU 가 에러코드를 push 하지 않는 벡터는 0 */
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
};

NX_STATIC_ASSERT(trap_frame_size, sizeof(struct trap_frame) == 176);

/* 인터럽트/예외 핸들러 타입 */
typedef void (*interrupt_handler_t)(struct trap_frame *frame);

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* GDT/TSS 를 만들어 로드한다. (UEFI 가 남긴 GDT 를 더 이상 쓰지 않는다) */
void gdt_init(void);

/* ring3 -> ring0 전환 시 CPU 가 사용할 커널 스택 최상단을 지정한다. */
void gdt_set_kernel_stack(u64 rsp0);

/* 256개 벡터 전체를 채운 IDT 를 만들어 로드한다. (인터럽트는 켜지지 않는다) */
void interrupt_init(void);

/* 벡터별 핸들러 등록/해제(NULL). 0x20~0x2F 는 irq_register_handler 를 사용 권장. */
Nstatus interrupt_register_handler(u32 vector, interrupt_handler_t handler);

/* PIC IRQ(0~15) 핸들러 등록. EOI 는 디스패처가 보낸다. */
Nstatus irq_register_handler(u8 irq, interrupt_handler_t handler);

/* 저수준: IDT 엔트리 하나를 채운다. (selector = SEL_KERNEL_CODE) */
void idt_set_gate(i32 vec, void *handler, struct idt_entry *idt, u8 flags, u8 ist);

/* 저수준: 지정한 IDT 를 lidt 로 로드한다. */
void idt_init(u16 limit, u64 base);

/* asm 스텁 진입점 (isr_stubs.s) */
void interrupt_dispatch(struct trap_frame *frame);

/* 예외 벡터 이름 (디버그 출력용) */
const char *exception_name(u64 vector);

#endif /* NYXIS_INTERRUPT_H */
