/*
 * start.s - 커널 진입점 (_start)
 *
 * NYTB 부트로더가 ExitBootServices() 이후 이 심볼로 점프한다.
 * 인자: rdi = NTBLI 포인터 (SysV ABI 첫 번째 인자)
 *
 * [수정 이력 요약]
 *  - Multiboot2 헤더 삭제: 이 커널은 64비트 코드로 시작하지만 Multiboot2 부트로더는
 *    32비트 보호 모드로 진입시키므로 어떤 경우에도 동작하지 않는 (그러나 GRUB 이
 *    "부팅 가능" 으로 오인하게 만드는) 헤더였다. UEFI 로더(NYTB)만 지원한다.
 *  - 스택을 고정 물리 주소(0x1000000)로 잡던 것을 커널 이미지 .bss 안의 전용 스택으로
 *    변경. 고정 주소는 펌웨어/부트로더가 그 물리 메모리를 이미 쓰고 있을 수 있다.
 *    스택 아래에 가드 페이지를 두어 paging_init 에서 매핑 해제한다.
 *  - 인터럽트를 끄고(cli), 방향 플래그를 지운다(cld). rbp 를 0 으로 (백트레이스 종단).
 *  - boot_info_bridge 호출 제거 (kernel.c 의 Multiboot 변환 코드와 함께 삭제).
 */

    .section .text
    .global _start
    .type _start, @function

_start:
    cli
    cld
    leaq boot_stack_top(%rip), %rsp     /* 16바이트 정렬된 스택 최상단 */
    xorq %rbp, %rbp
    call kernel_main                    /* rdi(NTBLI*) 는 그대로 전달 */

    /* kernel_main 은 반환하지 않는다. 혹시 반환하면 영구 정지 */
    cli
1:  hlt
    jmp 1b
    .size _start, . - _start

    /*
     * 부트 스택 (.bss). boot_stack_guard 페이지는 paging_init 에서 매핑 해제되어
     * 스택 오버플로가 발생하면 즉시 #PF -> (IST 를 쓰는) #DF 로 검출된다.
     */
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
