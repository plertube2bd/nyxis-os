#include "include/interrupt.h"

static interrupt_error_frame64_t* current_stack;
static interrupt_error_frame64_ring_t* current_stack_user;

struct idtr init_reg;
//struct idt_entry idt[256];

//

void idt_set_gate(
	i32 vec,
	void* handler,
	struct idt_entry* idt,
	u8 flags,
	u8 ist
) {
	u64 handler_addr = (u64)handler;
	idt[vec].ist         = ist;
	idt[vec].offset_low  = (u16)(handler_addr & 0xFFFF);
	idt[vec].offset_mid  = (u16)((handler_addr >> 16) & 0xFFFF);
	idt[vec].offset_high = (u32)((handler_addr >> 32) & 0xFFFFFFFF);
	idt[vec].selector    = 0x08;
	idt[vec].type_attr   = flags;
}
void idt_init(u16 limit, u64 base){
	init_reg.limit = limit;
	init_reg.base = base;
	__asm__ volatile (
		"lidt %0"
		:
		: "m"(init_reg)
	);
}
/*;void* idt_get_entry(){
	return idt;
*/

struct status find_current_status(void) {
	u8 return_type = 0;
	__asm__ volatile (
		"mov %%rsp, %0"
		: "=m"(current_stack)
	);
	if ((current_stack->cs & 0x03) == 0x03) {
		return_type = 1;
		current_stack_user = (interrupt_error_frame64_ring_t*)current_stack;
	}
	return (struct status){return_type, current_stack};
}
