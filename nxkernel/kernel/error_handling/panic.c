/*
 * panic.c - 커널 패닉
 *
 * [수정 이력 요약]
 *  - 메시지를 출력하기 "전에" 인터럽트를 끈다. (기존: 출력 도중 인터럽트가 끼어들 수 있음)
 *  - Message/error 인자를 실제로 출력한다. (기존에는 인자를 완전히 무시했다)
 *  - 헤더에 include 가 없어 Nstatus 를 모르던 문제 수정.
 *  - 프레임버퍼가 없어도 시리얼로 패닉 메시지가 남는다 (printk 가 시리얼 동시 출력).
 */

#include "nyxis.h"
#include "lowlevel.h"
#include "console/outputs/printk.h"
#include "console/outputs/conout.h"
#include "kernel/error_handling/panic.h"

void kernel_panic_simple(const char *Message, Nstatus error)
{
    cli();

    /* 장치 소유권(dev:/fb0, dev:/com1)과 무관하게 모든 출력으로 쓴다: 죽은 이유는 항상 보여야 한다 */
    conout_force_all_outputs();

    printk("\n");
    printk(KC_BOLD KC_RED "=========================================\n");
    printk(KC_BOLD KC_RED "             KERNEL PANIC!\n");
    printk(KC_BOLD KC_RED "=========================================\n");
    printk("%s\n", Message ? Message : "(no message)");
    printk("status: %r\n", error);
    printk("System halted.\n");

    for (;;) {
        cli();
        hlt();
    }
}
