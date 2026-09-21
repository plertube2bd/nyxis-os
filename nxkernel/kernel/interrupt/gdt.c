/*
 * gdt.c - 커널 자체 GDT / TSS 구성
 *
 * [왜 필요한가]
 *  이전 커널은 UEFI 펌웨어가 남긴 GDT 를 그대로 썼다. 그런데 IDT 게이트의 셀렉터를
 *  0x08 로 하드코딩했고 (OVMF 에서 64비트 코드 세그먼트는 0x38), ring3 진입용
 *  셀렉터(0x1B/0x23)도 UEFI GDT 에는 존재하지 않았다. 그 결과 인터럽트/예외/int 0x80
 *  이 발생하는 즉시 #GP -> #DF -> 트리플 폴트로 리셋될 수 있었다.
 *  또 펌웨어 메모리는 부트서비스 종료 후 재사용될 수 있으므로 커널이 자기 GDT 를
 *  가지는 것이 안전하다.
 *
 * GDT 레이아웃 (interrupt.h 의 SEL_* 와 일치해야 함):
 *   0x00 null
 *   0x08 커널 코드 (64비트, DPL0)
 *   0x10 커널 데이터 (DPL0)
 *   0x18 유저 코드 (32비트 호환 모드, DPL3) -> 셀렉터 0x1B   (SYSRET 규칙상 데이터보다 앞에 있어야 함)
 *   0x20 유저 데이터 (DPL3)                 -> 셀렉터 0x23
 *   0x28 유저 코드 (64비트, DPL3)           -> 셀렉터 0x2B
 *   0x30 TSS (16바이트 디스크립터: 0x30, 0x38 두 슬롯 차지)
 *
 * IST(Interrupt Stack Table): 더블폴트/NMI/머신체크는 스택이 망가진 상황에서도
 * 처리해야 하므로 전용 스택을 쓴다.
 */

#include "nyxis.h"
#include "interrupt.h"
#include "kernel/syscall/syscall.h"

#define GDT_ENTRIES 8   /* null, kcode, kdata, ucode32, udata, ucode64, tss(2) */

#define IST_STACK_SIZE 8192U

/* 세그먼트 디스크립터 값 (Base=0, Limit=0xFFFFF, G=1 ; 64비트 코드는 L=1) */
#define GDT_KERNEL_CODE64  0x00AF9A000000FFFFUL
#define GDT_KERNEL_DATA    0x00CF92000000FFFFUL
#define GDT_USER_CODE64    0x00AFFA000000FFFFUL
#define GDT_USER_CODE32    0x00CFFA000000FFFFUL
#define GDT_USER_DATA      0x00CFF2000000FFFFUL

static u64 g_gdt[GDT_ENTRIES] __attribute__((aligned(16)));
static struct tss64 g_tss __attribute__((aligned(16)));

static u8 g_ist_df_stack[IST_STACK_SIZE]  __attribute__((aligned(16)));
static u8 g_ist_nmi_stack[IST_STACK_SIZE] __attribute__((aligned(16)));
static u8 g_ist_mce_stack[IST_STACK_SIZE] __attribute__((aligned(16)));

/* TSS 디스크립터(16바이트)를 g_gdt[index], g_gdt[index+1] 에 채운다 */
static void gdt_set_tss(u32 index, u64 base, u32 limit)
{
    u64 low;

    low  = (u64)(limit & 0xFFFFU);
    low |= (base & 0xFFFFFFUL) << 16;
    low |= (u64)0x89 << 40;                  /* P=1, DPL=0, type=0x9 (사용 가능 64비트 TSS) */
    low |= (u64)((limit >> 16) & 0xFU) << 48;
    low |= ((base >> 24) & 0xFFUL) << 56;

    g_gdt[index]     = low;
    g_gdt[index + 1] = base >> 32;
}

void gdt_set_kernel_stack(u64 rsp0)
{
    g_tss.rsp0 = rsp0;                  /* int 0x80 / 예외 / IRQ 로 ring3 -> ring0 진입할 때 */
    g_cpu_local.kernel_rsp = rsp0;      /* syscall 명령으로 진입할 때 (syscall_entry.s) */
}

void gdt_init(void)
{
    struct idtr gdtr;   /* GDTR 과 IDTR 은 형식이 같다 (limit 16비트 + base 64비트) */

    g_tss.res1 = 0;
    g_tss.rsp0 = 0;      /* ring3 이 생기면 gdt_set_kernel_stack 으로 설정 */
    g_tss.rsp1 = 0;
    g_tss.rsp2 = 0;
    g_tss.res2 = 0;
    g_tss.ist1 = (u64)(usize)&g_ist_df_stack[IST_STACK_SIZE];
    g_tss.ist2 = (u64)(usize)&g_ist_nmi_stack[IST_STACK_SIZE];
    g_tss.ist3 = (u64)(usize)&g_ist_mce_stack[IST_STACK_SIZE];
    g_tss.ist4 = 0;
    g_tss.ist5 = 0;
    g_tss.ist6 = 0;
    g_tss.ist7 = 0;
    g_tss.res3 = 0;
    g_tss.res4 = 0;
    /* I/O 비트맵 없음: iomap_base 를 TSS 크기로 두면 ring3 의 포트 접근은 모두 #GP */
    g_tss.iomap_base = (u16)sizeof(g_tss);

    g_gdt[0] = 0;
    g_gdt[1] = GDT_KERNEL_CODE64;
    g_gdt[2] = GDT_KERNEL_DATA;
    g_gdt[3] = GDT_USER_CODE32;
    g_gdt[4] = GDT_USER_DATA;
    g_gdt[5] = GDT_USER_CODE64;
    gdt_set_tss(6, (u64)(usize)&g_tss, (u32)(sizeof(g_tss) - 1));

    gdtr.limit = (u16)(sizeof(g_gdt) - 1);
    gdtr.base  = (u64)(usize)g_gdt;

    /*
     * GDT 를 로드하고, 새 코드 세그먼트를 쓰도록 CS 를 far return 으로 다시 로드한 뒤
     * 데이터 세그먼트 레지스터를 갱신한다. 마지막으로 TSS 를 로드(ltr)한다.
     */
    __asm__ volatile (
        "lgdt %0\n\t"
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw %2, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "xorl %%eax, %%eax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %3, %%ax\n\t"
        "ltr %%ax"
        :
        : "m"(gdtr), "i"(SEL_KERNEL_CODE), "i"(SEL_KERNEL_DATA), "i"(SEL_TSS)
        : "rax", "memory"
    );
}
