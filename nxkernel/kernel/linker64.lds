/*
 * 64-bit Kernel Linker Script (higher-half)
 *
 * 메모리 레이아웃:
 *   [물리 1MiB] .boot   : 초기 진입 코드/데이터. 링크 주소 = 적재 주소 = 낮은 주소(1MiB~).
 *                         UEFI(NYTB)와 Multiboot2(GRUB) 양쪽 진입점이 여기 있고, 임시 페이지 테이블을 만든 뒤
 *                         높은 주소로 점프한다. 부팅 후에는 사용하지 않는다.
 *   [가상 0xFFFFFFFF80000000 + 물리] .text/.rodata/.data/.bss : 실제 커널.
 *                         적재(LMA)는 물리 주소 = VMA - KERNEL_VMA 이다.
 *
 * [수정 이력 요약]
 *  - PHDRS 로 세그먼트를 권한별(RX / R / RW)로 분리. (예전에는 하나의 RWX 세그먼트)
 *  - higher-half: VMA 와 LMA 분리 (부트로더는 p_paddr 에 적재하므로 코드 변경 없음).
 *  - 섹션 경계 심볼 제공 (paging.c 가 W^X 를 적용할 때 사용).
 *  - Multiboot2 헤더를 파일 앞쪽(32KiB 이내)에 배치. 진입점(ELF e_entry)은 64비트 UEFI 용 _start,
 *    GRUB 은 Multiboot2 헤더의 entry address 태그로 32비트 진입점 _start_mb 를 사용한다.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

PHDRS
{
    boot   PT_LOAD FLAGS(7);   /* RWX (초기 진입 코드 + 임시 페이지 테이블) */
    text   PT_LOAD FLAGS(5);   /* R-X */
    rodata PT_LOAD FLAGS(4);   /* R-- */
    data   PT_LOAD FLAGS(6);   /* RW- */
}

SECTIONS
{
    . = 0x100000;              /* 커널 물리 적재 주소: 1MiB */
    __boot_start = .;

    .boot : ALIGN(4K) {
        KEEP(*(.multiboot2_header))
        *(.boot.text)
        *(.boot.data)
        . = ALIGN(4K);
        *(.boot.bss)
        . = ALIGN(4K);
    } :boot

    __boot_end = .;

    /* 이후 섹션은 높은 가상 주소에 링크하되 물리 주소는 __boot_end 부터 이어 붙인다 */
    . += KERNEL_VMA;

    .text : AT(ADDR(.text) - KERNEL_VMA) ALIGN(4K) {
        __text_start = .;
        *(.text)
        *(.text.*)
        . = ALIGN(4K);
        __text_end = .;
    } :text

    .rodata : AT(ADDR(.rodata) - KERNEL_VMA) ALIGN(4K) {
        __rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4K);
        __rodata_end = .;
    } :rodata

    .data : AT(ADDR(.data) - KERNEL_VMA) ALIGN(4K) {
        __data_start = .;
        *(.data)
        *(.data.*)
    } :data

    .bss : AT(ADDR(.bss) - KERNEL_VMA) ALIGN(4K) {
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4K);
    } :data

    __kernel_start = __text_start;
    __kernel_end = .;

    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.note.gnu.property)
        *(.gnu_debuglink)
        *(.gnu_debugdata)
        *(.eh_frame)
        *(.comment)
    }
}
