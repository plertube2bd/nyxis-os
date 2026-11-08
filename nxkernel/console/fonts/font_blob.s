/*
 * font_blob.s - 콘솔용 TrueType 폰트 파일을 커널 .rodata 에 그대로 넣는다 (CONFIG_FONT_8x16).
 *
 * 경로는 make 가 실행되는 nxkernel/ 기준이다. 폰트를 바꾸려면 이 파일과 Makefile 의 의존성(FONT_TTF)을 함께 바꾼다.
 * 폰트: Hack Regular (MIT + Bitstream Vera License, console/fonts/Hack-LICENSE.md)
 */
    .section .rodata
    .balign 16
    .global nx_console_ttf_start
    .global nx_console_ttf_end
nx_console_ttf_start:
    .incbin "console/fonts/Hack-Regular.ttf"
nx_console_ttf_end:

    .section .note.GNU-stack,"",@progbits
