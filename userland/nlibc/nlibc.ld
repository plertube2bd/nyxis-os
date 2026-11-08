/*
 * nlibc.ld - nlibc 프로그램용 링커 스크립트 (Nyxis 정적 ELF64 ET_EXEC)
 *
 * 커널 ELF 로더(nxkernel/kernel/process/elf.h)의 요구:
 *   - 정적 ET_EXEC (동적 링크/PIE 없음), PT_LOAD 만 의미가 있다
 *   - PT_LOAD 의 p_vaddr 는 4KiB 정렬, 0x10000 이상
 *   - 쓰기 + 실행 세그먼트 금지 (W^X)
 * 그래서 세그먼트를 권한별로 나누고 각각 새 페이지에서 시작한다:
 *   text (R-X) : .text            rodata (R--) : .rodata          data (RW-) : .data .bss
 * 힙(brk)은 이미지 끝(마지막 세그먼트 끝을 4KiB 로 올린 곳)에서 시작한다 — 커널이 정한다.
 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);    /* R-X */
    rodata PT_LOAD FLAGS(4);    /* R-- */
    data   PT_LOAD FLAGS(6);    /* RW- */
}

SECTIONS
{
    . = 0x400000;

    .text : ALIGN(4K) {
        *(.text.startup .text.startup.*)
        *(.text .text.*)
    } :text

    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
    } :rodata

    .data : ALIGN(4K) {
        *(.data .data.*)
    } :data

    .bss : ALIGN(16) {
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4K);
    } :data

    _end = .;

    /* 정적 C89 프로그램에 필요 없는 것들 (예외 처리용 되감기 표, 빌드 메모, 생성자 표는 쓰지 않는다) */
    /DISCARD/ : {
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr .eh_frame.*)
        *(.comment)
        *(.gnu.attributes .gnu.version*)
        *(.init_array* .fini_array* .ctors* .dtors*)
    }
}
