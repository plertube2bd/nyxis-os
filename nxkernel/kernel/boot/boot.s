/*
 * boot.s - 커널 초기 진입 코드 (UEFI 64비트 진입 + Multiboot2 32비트 진입 -> higher-half)
 *
 * 진입 경로:
 *   1) UEFI (NYTB 부트로더):  _start      (64비트 롱 모드, rdi = NTBLI 물리 주소)
 *   2) GRUB (Multiboot2):     _start_mb   (32비트 보호 모드, eax = 0x36D76289, ebx = MBI 물리 주소)
 *
 * 두 경로 모두 다음을 수행한 뒤 kernel_main(boot_kind, info_phys) 를 높은 주소에서 호출한다.
 *   - 부트 정보(및 UEFI 메모리 맵)를 커널 이미지 안의 버퍼로 복사 (UEFI 의 페이지 테이블을 버리기 전에)
 *   - 임시 페이지 테이블 구성:  낮은 4GiB 항등 매핑 + 커널 이미지를 0xFFFFFFFF80000000 에 매핑
 *   - CR3 교체, 높은 주소의 부트 스택으로 전환, kernel_main 으로 점프
 *
 * 이 파일의 코드/데이터는 .boot.* 섹션(낮은 주소에 링크)에 있다. 높은 주소의 커널 심볼(boot_stack_top,
 * kernel_main)은 CR3 교체 후에만 접근/점프한다.
 */

    .set KVMA,          0xFFFFFFFF80000000
    .set NTBLI_SIZE,    112
    .set NTBLI_MEMMAP_BASE, 72
    .set NTBLI_MEMMAP_SIZE, 80
    .set MEMMAP_MAX,    65536
    .set BOOT_UEFI,     1
    .set BOOT_MULTIBOOT2, 2

/* ------------------------------------------------------------------ */
/* Multiboot2 헤더 (파일의 처음 32KiB 안에 있어야 한다)                */
/* ------------------------------------------------------------------ */
    .section .multiboot2_header, "a"
    .align 8
mb2_header_start:
    .long 0xE85250D6                                    /* magic */
    .long 0                                             /* architecture: i386 (32비트 보호 모드 진입) */
    .long mb2_header_end - mb2_header_start             /* header length */
    .long -(0xE85250D6 + 0 + (mb2_header_end - mb2_header_start))   /* checksum */

    .align 8                                            /* entry address 태그: 32비트 진입점 */
    .short 3
    .short 0
    .long 12
    .long _start_mb

    .align 8                                            /* framebuffer 태그: 1024x768x32 선호 */
    .short 5
    .short 0
    .long 20
    .long 1024
    .long 768
    .long 32

    .align 8                                            /* 모듈을 4KiB 정렬로 적재 */
    .short 6
    .short 0
    .long 8

    .align 8                                            /* 종료 태그 */
    .short 0
    .short 0
    .long 8
mb2_header_end:

/* ------------------------------------------------------------------ */
/* 초기 페이지 테이블 / 버퍼 (.boot.bss: 0 으로 초기화됨)              */
/* ------------------------------------------------------------------ */
    .section .boot.bss, "aw", @nobits
    .align 4096
early_pml4:     .skip 4096
early_pdpt_lo:  .skip 4096
early_pd_lo:    .skip 4096 * 4          /* 낮은 4GiB (2MiB 페이지 2048개) */
early_pdpt_hi:  .skip 4096
early_pd_hi:    .skip 4096              /* 커널 이미지 (VMA 0xFFFFFFFF80000000~) */
    .align 16
early_ntbli:    .skip 128               /* NTBLI 사본 */
early_memmap:   .skip MEMMAP_MAX        /* UEFI 메모리 맵 사본 */
    .align 16
early_stack:    .skip 4096
early_stack_top:


/* ------------------------------------------------------------------ */
/* Multiboot2 임시 GDT                                                 */
/* ------------------------------------------------------------------ */
    .section .boot.data, "aw"
    .align 16
boot_gdt:
    .quad 0x0000000000000000
    .quad 0x00AF9A000000FFFF            /* 0x08: 64비트 코드 */
    .quad 0x00CF92000000FFFF            /* 0x10: 데이터 */
boot_gdt_desc:
    .short boot_gdt_desc - boot_gdt - 1
    .long  boot_gdt
    .long  0

mb_err_msg:
    .asciz "NYXIS: 64-BIT CPU REQUIRED / BAD MULTIBOOT2"

/* ------------------------------------------------------------------ */
/* 1) UEFI 진입점 (64비트)                                             */
/* ------------------------------------------------------------------ */
    .section .boot.text, "ax"
    .code64
    .global _start
    .type _start, @function
_start:
    cli
    cld

    /* NTBLI 사본 (UEFI 페이지 테이블이 아직 살아 있어서 rdi 를 그대로 읽을 수 있다) */
    movq %rdi, %rsi
    leaq early_ntbli(%rip), %rdi
    movl $NTBLI_SIZE, %ecx
    rep movsb

    /* 메모리 맵 사본: 크기를 MEMMAP_MAX 로 제한 */
    movq early_ntbli + NTBLI_MEMMAP_BASE(%rip), %rsi
    movq early_ntbli + NTBLI_MEMMAP_SIZE(%rip), %rcx
    testq %rsi, %rsi
    jnz 1f
    xorl %ecx, %ecx
1:  cmpq $MEMMAP_MAX, %rcx
    jbe 2f
    movl $MEMMAP_MAX, %ecx
2:  movq %rcx, early_ntbli + NTBLI_MEMMAP_SIZE(%rip)
    leaq early_memmap(%rip), %rdi
    rep movsb
    leaq early_memmap(%rip), %rax
    movq %rax, early_ntbli + NTBLI_MEMMAP_BASE(%rip)

    movl $BOOT_UEFI, %edi
    leaq early_ntbli(%rip), %rsi
    jmp stage1_common
    .size _start, . - _start

/* ------------------------------------------------------------------ */
/* 공통 (64비트): 임시 페이지 테이블 구성 -> higher-half 진입           */
/*   edi = boot kind, rsi = 부트 정보 물리 주소                        */
/* ------------------------------------------------------------------ */
    .type stage1_common, @function
stage1_common:
    movq %rsi, %r12
    movl %edi, %r13d

    /* PML4[0] -> pdpt_lo (낮은 4GiB), PML4[511] -> pdpt_hi (커널) */
    leaq early_pml4(%rip), %rdi
    leaq early_pdpt_lo(%rip), %rax
    orq $3, %rax
    movq %rax, (%rdi)
    leaq early_pdpt_hi(%rip), %rax
    orq $3, %rax
    movq %rax, 511*8(%rdi)

    /* pdpt_lo[0..3] -> pd_lo + i*4096 */
    leaq early_pdpt_lo(%rip), %rdi
    leaq early_pd_lo(%rip), %rax
    orq $3, %rax
    movl $4, %ecx
3:  movq %rax, (%rdi)
    addq $4096, %rax
    addq $8, %rdi
    decl %ecx
    jnz 3b

    /* pd_lo: 2MiB 페이지 2048개로 낮은 4GiB 를 항등 매핑 (P|RW|PS) */
    leaq early_pd_lo(%rip), %rdi
    movq $0x83, %rax
    movl $2048, %ecx
4:  movq %rax, (%rdi)
    addq $0x200000, %rax
    addq $8, %rdi
    decl %ecx
    jnz 4b

    /* pdpt_hi[510] -> pd_hi (VMA 0xFFFFFFFF80000000 = PML4[511], PDPT[510]) */
    leaq early_pdpt_hi(%rip), %rdi
    leaq early_pd_hi(%rip), %rax
    orq $3, %rax
    movq %rax, 510*8(%rdi)

    /* pd_hi: 물리 0 ~ 64MiB 를 VMA 0xFFFFFFFF80000000 ~ 에 매핑 (임시, RWX) */
    leaq early_pd_hi(%rip), %rdi
    movq $0x83, %rax
    movl $32, %ecx
5:  movq %rax, (%rdi)
    addq $0x200000, %rax
    addq $8, %rdi
    decl %ecx
    jnz 5b

    /* 새 페이지 테이블로 전환 (이 코드는 낮은 주소이며 항등 매핑되어 있으므로 계속 실행 가능) */
    leaq early_pml4(%rip), %rax
    movq %rax, %cr3

    /* 높은 주소의 부트 스택으로 이동 후 kernel_main(kind, info_phys) 로 점프 */
    movabsq $boot_stack_top, %rsp
    xorl %ebp, %ebp
    movl %r13d, %edi
    movq %r12, %rsi
    pushq $0                        /* 가짜 반환 주소: 진입 시 rsp 정렬 규약(8 mod 16) 맞춤 */
    movabsq $kernel_main, %rax
    jmp *%rax
    .size stage1_common, . - stage1_common

/* ------------------------------------------------------------------ */
/* 2) Multiboot2 진입점 (32비트 보호 모드)                             */
/* ------------------------------------------------------------------ */
    .code32
    .global _start_mb
    .type _start_mb, @function
_start_mb:
    cli
    cmpl $0x36D76289, %eax
    jne mb_fail
    movl %ebx, %esi                     /* MBI 물리 주소 보관 (64비트 진입 후 rsi) */
    movl $early_stack_top, %esp

    /* 64비트(long mode) 지원 확인 */
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb mb_fail
    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz mb_fail

    /* 낮은 4GiB 항등 매핑 (롱 모드 진입용 최소 테이블) */
    movl $early_pdpt_lo, %eax
    orl $3, %eax
    movl %eax, early_pml4
    movl $early_pd_lo, %eax
    orl $3, %eax
    movl $early_pdpt_lo, %edi
    movl $4, %ecx
1:  movl %eax, (%edi)
    addl $4096, %eax
    addl $8, %edi
    decl %ecx
    jnz 1b
    movl $early_pd_lo, %edi
    movl $0x83, %eax
    movl $2048, %ecx
2:  movl %eax, (%edi)
    addl $0x200000, %eax
    addl $8, %edi
    decl %ecx
    jnz 2b

    /* PAE -> CR3 -> LME -> PG */
    movl %cr4, %eax
    orl $0x20, %eax
    movl %eax, %cr4
    movl $early_pml4, %eax
    movl %eax, %cr3
    movl $0xC0000080, %ecx
    rdmsr
    orl $0x100, %eax
    wrmsr
    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0

    lgdt boot_gdt_desc
    ljmp $0x08, $_start_mb64

mb_fail:
    /* VGA 텍스트 모드(0xB8000)에 오류 문구를 쓰고 정지 */
    movl $mb_err_msg, %esi
    movl $0xB8000, %edi
3:  movzbl (%esi), %eax
    testb %al, %al
    jz 4f
    orl $0x4F00, %eax                   /* 흰색 글자 / 빨간 배경 */
    movw %ax, (%edi)
    incl %esi
    addl $2, %edi
    jmp 3b
4:  cli
    hlt
    jmp 4b
    .size _start_mb, . - _start_mb

    .code64
_start_mb64:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorl %eax, %eax
    movw %ax, %fs
    movw %ax, %gs
    movl %esi, %esi                     /* MBI 주소 상위 32비트를 0 으로 */
    movl $BOOT_MULTIBOOT2, %edi
    jmp stage1_common

/* ------------------------------------------------------------------ */
/* 높은 주소의 부트 스택 (.bss). 가드 페이지는 paging_init 에서 매핑 해제 */
/* ------------------------------------------------------------------ */
    .section .bss
    .align 4096
    .global boot_stack_guard
boot_stack_guard:
    .skip 4096
    .align 16
    .global boot_stack_bottom
boot_stack_bottom:
    .skip 32768
    .global boot_stack_top
boot_stack_top:

    .section .note.GNU-stack,"",@progbits
