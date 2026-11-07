/*
 * kheap.c - 커널 힙 구현 (설계는 kheap.h 참고)
 */

#include "kernel/mm/kheap.h"
#include "kernel/mm/pfa.h"
#include "kernel/error_handling/panic.h"
#include "phys.h"
#include "memory.h"
#include "lowlevel.h"

#define KHEAP_PAGE_SIZE   4096UL
#define KHEAP_MAGIC       0x4B484550U       /* "KHEP" */
#define KHEAP_HDR_SIZE    64UL
#define KHEAP_CLASSES     8U
#define KHEAP_BITMAP_BITS 256U

typedef struct slab_page {
    u32 magic;
    u16 class_idx;
    u16 in_use;
    u16 capacity;
    u16 obj_size;
    u32 reserved;
    struct slab_page *next;      /* 같은 클래스의 "빈 칸이 있는" 페이지 목록 */
    struct slab_page *prev;
    u64 bitmap[KHEAP_BITMAP_BITS / 64U];
} slab_page_t;

NX_STATIC_ASSERT(kheap_header_size, sizeof(slab_page_t) == KHEAP_HDR_SIZE);
/* 가장 작은 클래스의 칸 수가 비트맵에 들어가야 한다: (4096-64)/16 = 252 <= 256 */
NX_STATIC_ASSERT(kheap_bitmap_fits, (KHEAP_PAGE_SIZE - KHEAP_HDR_SIZE) / 16UL <= KHEAP_BITMAP_BITS);

static const u16 g_class_size[KHEAP_CLASSES] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };
static slab_page_t *g_partial[KHEAP_CLASSES];
static u64 g_live_objects = 0;
static u64 g_slab_pages = 0;

static void list_remove(u32 cls, slab_page_t *pg)
{
    if (pg->prev)
        pg->prev->next = pg->next;
    else
        g_partial[cls] = pg->next;
    if (pg->next)
        pg->next->prev = pg->prev;
    pg->next = nNULL;
    pg->prev = nNULL;
}

static void list_push(u32 cls, slab_page_t *pg)
{
    pg->prev = nNULL;
    pg->next = g_partial[cls];
    if (g_partial[cls])
        g_partial[cls]->prev = pg;
    g_partial[cls] = pg;
}

static slab_page_t *new_slab(u32 cls)
{
    u64 phys;
    slab_page_t *pg;

    if (NSTATUS_IS_ERR(pfa_alloc(&phys)))
        return nNULL;

    pg = (slab_page_t *)phys_to_virt(phys);        /* pfa_alloc 이 0 으로 채워 준다 */
    pg->magic = KHEAP_MAGIC;
    pg->class_idx = (u16)cls;
    pg->obj_size = g_class_size[cls];
    pg->capacity = (u16)((KHEAP_PAGE_SIZE - KHEAP_HDR_SIZE) / g_class_size[cls]);
    g_slab_pages++;
    return pg;
}

void *kmalloc(usize size)
{
    u32 cls;
    u32 i;
    slab_page_t *pg;
    u64 flags;
    void *obj = nNULL;

    if (size == 0 || size > KHEAP_MAX_ALLOC)
        return nNULL;

    for (cls = 0; cls < KHEAP_CLASSES; cls++) {
        if (size <= g_class_size[cls])
            break;
    }

    flags = irq_save();

    pg = g_partial[cls];
    if (!pg) {
        pg = new_slab(cls);
        if (!pg) {
            irq_restore(flags);
            return nNULL;
        }
        list_push(cls, pg);
    }

    for (i = 0; i < pg->capacity; i++) {
        u64 bit = 1UL << (i & 63U);

        if (!(pg->bitmap[i >> 6] & bit)) {
            pg->bitmap[i >> 6] |= bit;
            pg->in_use++;
            obj = (u8 *)pg + KHEAP_HDR_SIZE + (usize)i * pg->obj_size;
            break;
        }
    }

    if (!obj)
        kernel_panic_simple("kmalloc: partial slab has no free slot (heap corruption)", NheapCorruption);

    if (pg->in_use == pg->capacity)
        list_remove(cls, pg);         /* 가득 찼다: 더 이상 "빈 칸이 있는" 목록에 두지 않는다 */

    g_live_objects++;
    irq_restore(flags);
    return obj;
}

void *kzalloc(usize size)
{
    void *p = kmalloc(size);

    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *ptr)
{
    slab_page_t *pg;
    usize off;
    u32 idx;
    u64 bit;
    u64 flags;

    if (!ptr)
        return;

    pg = (slab_page_t *)(usize)((u64)(usize)ptr & ~(KHEAP_PAGE_SIZE - 1));

    flags = irq_save();

    if (pg->magic != KHEAP_MAGIC || pg->class_idx >= KHEAP_CLASSES ||
        pg->obj_size != g_class_size[pg->class_idx])
        /* 마지막 칸이 해제되어 페이지가 PFA 로 돌아간 뒤의 이중 해제도 여기로 온다 (PFA 가 페이지 머리를 덮어쓴다) */
        kernel_panic_simple("kfree: not a live kernel heap object (double free of a released page, or not from kmalloc)",
                            NinvalidPointer);

    off = (usize)((u8 *)ptr - (u8 *)pg);
    if (off < KHEAP_HDR_SIZE || (off - KHEAP_HDR_SIZE) % pg->obj_size != 0)
        kernel_panic_simple("kfree: pointer is not the start of a heap object", NinvalidPointer);

    idx = (u32)((off - KHEAP_HDR_SIZE) / pg->obj_size);
    bit = 1UL << (idx & 63U);
    if (idx >= pg->capacity || !(pg->bitmap[idx >> 6] & bit))
        kernel_panic_simple("kfree: double free", NdoubleFree);

    memset(ptr, 0xDE, pg->obj_size);
    pg->bitmap[idx >> 6] &= ~bit;
    if (pg->in_use == pg->capacity)
        list_push(pg->class_idx, pg);  /* 가득 찼던 페이지에 빈 칸이 생겼다 */
    pg->in_use--;
    g_live_objects--;

    if (pg->in_use == 0) {
        list_remove(pg->class_idx, pg);
        pg->magic = 0;                  /* 이후 이 페이지를 가리키는 kfree 는 거부된다 */
        g_slab_pages--;
        (void)pfa_free(virt_to_phys(pg));
    }

    irq_restore(flags);
}

void *kpage_alloc(void)
{
    u64 phys;

    if (NSTATUS_IS_ERR(pfa_alloc(&phys)))
        return nNULL;
    return phys_to_virt(phys);
}

void kpage_free(void *page)
{
    if (!page)
        return;
    if (!virt_is_direct(page) || ((u64)(usize)page & (KHEAP_PAGE_SIZE - 1)) != 0)
        kernel_panic_simple("kpage_free: not a page-aligned HHDM address", NinvalidPointer);
    (void)pfa_free(virt_to_phys(page));
}

u64 kheap_live_objects(void)
{
    return g_live_objects;
}

u64 kheap_slab_pages(void)
{
    return g_slab_pages;
}
