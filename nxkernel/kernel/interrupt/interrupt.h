#ifndef KERNEL_INTERRUPT_H
#define KERNEL_INTERRUPT_H

#include "../../include/interrupt.h"

static interrupt_error_frame64_t* current_stack;
static interrupt_error_frame64_ring_t* current_stack_user;

struct idtr init_reg;
// struct idt_entry idt[256];

void idt_set_gate(
	i32 vec,
	void* handler,
	struct idt_entry* idt,
	u8 flags,
	u8 ist
);

void idt_init(u16 limit, u64 base);

// void* idt_get_entry();

struct status find_current_status(void);

#endif // KERNEL_INTERRUPT_H