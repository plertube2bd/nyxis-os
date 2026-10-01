/*
 * elftest 링커 스크립트: 정적 ET_EXEC, 고정 주소(0x400000).
 * 세그먼트를 권한별로 나누고 각각 4KiB 페이지에 정렬한다 (커널 ELF 로더의 요구사항).
 *   .text = R-X, .rodata = R--, .data/.bss = RW-
 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x400000;

    .text : ALIGN(4K) { *(.text) *(.text.*) } :text

    .rodata : ALIGN(4K) { *(.rodata) *(.rodata.*) } :rodata

    .data : ALIGN(4K) { *(.data) *(.data.*) } :data
    .bss : ALIGN(4K) {
        *(.bss) *(.bss.*) *(COMMON)
        . = ALIGN(4K);
    } :data

    /DISCARD/ : { *(.note.*) *(.eh_frame) *(.comment) }
}
