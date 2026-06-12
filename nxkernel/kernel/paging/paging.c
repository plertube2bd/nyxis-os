#include "kernel/paging/paging.h"
#include "memory.h"
#include "lowlevel.h"

#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4

#define PAGE_SIZE    4096

static page_directory_t* current_page_directory = nNULL;
static u8* paging_alloc_ptr = (u8*)0x00200000;

static void* kmalloc_aligned(usize size, usize align) {
    usize addr = (usize)paging_alloc_ptr;

    if (addr & (align - 1)) {
        addr = (addr + align - 1) & ~(align - 1);
    }

    paging_alloc_ptr = (u8*)(addr + size);
    return (void*)addr;
}

/* ------------------------------------------------ */
/* Helpers                                          */
/* ------------------------------------------------ */

static inline void paging_flush_tlb(void* addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

static inline void paging_reload_cr3() {
    __asm__ volatile(
        "mov %%cr3, %%rax\n"
        "mov %%rax, %%cr3\n"
        :
        :
        : "rax", "memory"
    );
}

/* ------------------------------------------------ */
/* Initialize paging                                */
/* ------------------------------------------------ */

Nstatus paging_init() {

    /* Allocate aligned page directory */
    current_page_directory =
        (page_directory_t*)kmalloc_aligned(sizeof(page_directory_t), PAGE_SIZE);

    if (!current_page_directory)
        return NoutOfMemory;

    memset(current_page_directory, 0, sizeof(page_directory_t));

    /* Allocate first page table */
    page_table_t* first_table =
        (page_table_t*)kmalloc_aligned(sizeof(page_table_t), PAGE_SIZE);

    if (!first_table)
        return NoutOfMemory;

    memset(first_table, 0, sizeof(page_table_t));

    /*
        Identity map first 4MB
        0x00000000 ~ 0x003FFFFF
    */

    for (u32 i = 0; i < 1024; i++) {
        first_table->entries[i].present = 1;
        first_table->entries[i].rw = 1;
        first_table->entries[i].user = 0;
        first_table->entries[i].frame = i;
    }

    /* PDE[0] -> first_table */
    current_page_directory->entries[0].present = 1;
    current_page_directory->entries[0].rw = 1;
    current_page_directory->entries[0].user = 0;
    current_page_directory->entries[0].frame =
        ((u32)first_table) >> 12;

    return NSTATUS_OK;
}

/* ------------------------------------------------ */
/* Get/Create page table                            */
/* ------------------------------------------------ */

static page_table_t* paging_get_table(u32 virt_addr, u32 create) {

    u32 pd_index = virt_addr >> 22;

    page_directory_entry_t* pde =
        &current_page_directory->entries[pd_index];

    /* Table exists */
    if (pde->present) {
        return (page_table_t*)(pde->frame << 12);
    }

    /* Create disabled */
    if (!create)
        return nNULL;

    /* Allocate new table */
    page_table_t* table =
        (page_table_t*)kmalloc_aligned(sizeof(page_table_t), PAGE_SIZE);

    if (!table)
        return nNULL;

    memset(table, 0, sizeof(page_table_t));

    pde->present = 1;
    pde->rw = 1;
    pde->user = 0;
    pde->frame = ((u32)table) >> 12;

    return table;
}

/* ------------------------------------------------ */
/* Map page                                         */
/* ------------------------------------------------ */

Nstatus paging_map_page(void* phys, void* virt, u32 flags) {

    if (!phys || !virt)
        return NinvalidArg;

    u32 virt_addr = (u32)virt;
    u32 phys_addr = (u32)phys;

    u32 pt_index = (virt_addr >> 12) & 0x3FF;

    page_table_t* table =
        paging_get_table(virt_addr, 1);

    if (!table)
        return NinvalidArg;

    table->entries[pt_index].present = 1;
    table->entries[pt_index].rw = (flags & PAGE_RW) ? 1 : 0;
    table->entries[pt_index].user = (flags & PAGE_USER) ? 1 : 0;
    table->entries[pt_index].frame = phys_addr >> 12;

    paging_flush_tlb(virt);

    return NSTATUS_OK;
}

/* ------------------------------------------------ */
/* Unmap page                                       */
/* ------------------------------------------------ */

Nstatus paging_unmap_page(void* virt) {

    if (!virt)
        return NinvalidArg;

    u32 virt_addr = (u32)virt;

    u32 pd_index = virt_addr >> 22;
    u32 pt_index = (virt_addr >> 12) & 0x3FF;

    page_directory_entry_t* pde =
        &current_page_directory->entries[pd_index];

    if (!pde->present)
        return NinvalidArg;

    page_table_t* table =
        (page_table_t*)(pde->frame << 12);

    table->entries[pt_index].present = 0;
    table->entries[pt_index].frame = 0;

    paging_flush_tlb(virt);

    return NSTATUS_OK;
}

/* ------------------------------------------------ */
/* Enable paging                                    */
/* ------------------------------------------------ */

void paging_enable() {

    __asm__ volatile(
        "mov %0, %%cr3\n"
        :
        : "r"(current_page_directory)
        : "memory"
    );

    __asm__ volatile(
        "mov %%cr0, %%rax\n"
        "orq $-2147483648, %%rax\n"
        "mov %%rax, %%cr0\n"
        :
        :
        : "rax", "memory"
    );

    paging_reload_cr3();
}

/* ------------------------------------------------ */
/* Disable paging                                   */
/* ------------------------------------------------ */

void paging_disable() {

    __asm__ volatile(
        "mov %%cr0, %%rax\n"
        "andq $0x7FFFFFFF, %%rax\n"
        "mov %%rax, %%cr0\n"
        :
        :
        : "rax", "memory"
    );
}