/*
 * 64-bit Kernel Linker Script
 *
 * 커널은 물리 주소 1MiB 에 링크되며 (항등 매핑) NYTB 부트로더가 각 PT_LOAD 세그먼트를
 * p_paddr 위치에 그대로 올린다.
 *
 * [수정 이력 요약]
 *  - PHDRS 로 세그먼트를 권한별(RX / R / RW)로 분리. 예전에는 하나의 RWX 세그먼트였다.
 *  - 섹션 경계 심볼 제공 (paging.c 가 W^X 를 적용할 때 사용).
 *  - 각 세그먼트는 4KiB 로 정렬되어 서로 페이지를 공유하지 않는다.
 *  - .multiboot2_header 삭제 (start.s 참고).
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R-X */
    rodata PT_LOAD FLAGS(4);   /* R-- */
    data   PT_LOAD FLAGS(6);   /* RW- */
}

SECTIONS
{
    . = 0x100000;  /* 커널 시작: 1MiB */
    __kernel_start = .;

    .text : ALIGN(4K) {
        __text_start = .;
        *(.text)
        *(.text.*)
        . = ALIGN(4K);
        __text_end = .;
    } :text

    .rodata : ALIGN(4K) {
        __rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4K);
        __rodata_end = .;
    } :rodata

    .data : ALIGN(4K) {
        __data_start = .;
        *(.data)
        *(.data.*)
    } :data

    .bss : ALIGN(4K) {
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4K);
    } :data

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
