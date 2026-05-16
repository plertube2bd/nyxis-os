//this is for 32bit
#ifndef INTURRUPT_H
#define INTURRUPT_H
#include "include/types.h"
struct idtr{
	u16 limit;
	u64 base ;
}pack;
struct idt_entry{
	u16 offset_low ;
	u16 selector   ;
	u8  ist        ;
	u8  type_attr  ;
	u16 offset_mid ;
	u32 offset_high;
	u32 zero64     ;
}pack;
#define idtr_limit 0 /*16*/
#define idtr_base 2 /*32 or 64*/
#define idt_entry_offset_low 0 /*16*/
#define idt_entry_selector 2 /*16*/
#define idt_entry_zero 4 /*8*/
#define idt_entry_type_attr 5 /*8*/
#define idt_entry_offset_high 6 /*16*/
//64bit extand
#define idt_entry_offset_mid 6 /*16*/
#define idt_entry_offset_high_64 8 /*32*/
#define idt_entry_zerof64 12 /*32*/
/*addr = offset_low | (offset_high << 16);
addr = offset_low | ((unsigned long)offset_mid << 16) | ((unsigned long)offset+high << 32);
// selector
// kernel code segment 0x08
*/
/*type attr
7 Present
6:5 DPL
4 0
3:0 Gate Type

0x8E 32bit Interrupt Gate
0x8F Trap Gate
0x85 Task Gate
*/

typedef struct interrupt_frame32 {
	u32 eip;
	u32 cs;
	u32 eflags;
} interrupt_frame32_t;

typedef struct interrupt_error_frame32 {
	u32 error_code;
	u32 eip;
	u32 cs;
	u32 eflags;
} interrupt_error_frame32_t;

typedef struct interrupt_frame32_ring {
	u32 eip;
	u32 cs;
	u32 eflags;
	u32 esp;
	u32 ss;
} interrupt_frame32_ring_t;

typedef struct inturrupt_frame64 {
	u64 rip;
	u64 cs;
	u64 rflags;
} interrupt_frame64_t;

typedef struct inturrupt_frame64_ring {
	u64 rip;
	u64 cs;
	u64 rflags;
	u64 rsp;
	u64 ss;
} interrupt_frame64_ring_t;

typedef struct inturrupt_error_frame64 {
	u64 error_code;
	u64 rip;
	u64 cs;
	u64 rflags;
	u64 rsp;
	u64 ss;
} interrupt_error_frame64_t;

typedef struct inturrupt_error_frame64_ring {
	u64 error_code;
	u64 rip;
	u64 cs;
	u64 rflags;
	u64 rsp;
	u64 ss;
} interrupt_error_frame64_ring_t;

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
}pack;

struct regs64 {
	u64 r15;
	u64 r14;
	u64 r13;
	u64 r12;
	u64 r11;
	u64 r10;
	u64 r9;
	u64 r8;
	
	u64 rbp;
	u64 rdi;
	u64 rsi;
	u64 rdx;
	u64 rcx;
	u64 rbx;
	u64 rax;
};
struct status {u8 type; void* pointer;};

void idt_set_gate(i32, void*, struct idt_entry*, u8, u8);
void idt_init(u16, u64);
void* idt_get_entry(void);

interrupt void zero_div(void);
struct status find_current_status(void);

//must writing out a functions about the GDT!!!

#endif
