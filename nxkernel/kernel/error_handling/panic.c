/*
 * panic.c - 커널 패닉
 *
 * [수정 이력 요약]
 *  - 메시지를 출력하기 "전에" 인터럽트를 끈다. (기존: 출력 도중 인터럽트가 끼어들 수 있음)
 *  - Message/error 인자를 실제로 출력한다. (기존에는 인자를 완전히 무시했다)
 *  - 헤더에 include 가 없어 Nstatus 를 모르던 문제 수정.
 *  - 프레임버퍼가 없어도 시리얼로 패닉 메시지가 남는다 (printk 가 시리얼 동시 출력).
 *  - kernel_panic_frame: 패닉 당시의 CPU 상태와 프로세스도 찍는다.
 *  - SysRq: Alt+SysRq+C (PS/2) 또는 시리얼 BREAK 다음 'c' 로 강제 패닉 (sysrq_key).
 */

#include "nyxis.h"
#include "lowlevel.h"
#include "console/outputs/printk.h"
#include "console/outputs/conout.h"
#include "console/outputs/ansi.h"
#include "interrupt.h"
#include "kernel/process/process.h"
#include "kernel/error_handling/panic.h"

/* 패닉에 자주 쓰는 상태의 이름 (나머지는 숫자만) */
static const char *panic_status_name(Nstatus error)
{
    if (error == Npanic)
        return " (Npanic)";
    if (error == NkernelFault)
        return " (NkernelFault)";
    if (error == Nfatal)
        return " (Nfatal)";
    return "";
}

void kernel_panic_frame(const char *Message, Nstatus error, const struct trap_frame *frame)
{
    process_t *p;

    cli();

    /* 장치 소유권(dev:/fb0, dev:/com1)과 무관하게 모든 출력으로 쓴다: 죽은 이유는 항상 보여야 한다 */
    conout_force_all_outputs();

    /* 화면(프레임버퍼)은 비우고 맨 위부터: TUI 가 남긴 색이나 스크롤에 묻히지 않게. 시리얼 로그는 그대로 이어 쓴다 */
    ansi_reset();                       /* 사용자 출력 쪽 색(TUI 의 배경 등)을 잊는다: printk 가 끝날 때 되살리지 않게 */
    conout_reset_color();
    conout_erase_display(2);
    conout_set_cursor(0, 0);
    conout_set_cursor_visible(false);

    printk("\n");
    printk(KC_BOLD KC_RED "=========================================\n");
    printk(KC_BOLD KC_RED "             KERNEL PANIC!\n");
    printk(KC_BOLD KC_RED "=========================================\n");
    printk("%s\n", Message ? Message : "(no message)");
    printk("status: %r%s\n", error, panic_status_name(error));

    if (frame != nNULL) {
        p = current_process;
        printk("\n-- context (interrupted %s code) --\n", (frame->cs & 3UL) == 3UL ? "user" : "kernel");
        if (p != nNULL && p->pid == 0)
            printk("process: idle (pid 0)\n");
        else if (p != nNULL)
            printk("process: pid %u, user %s (uid %u)\n", p->pid,
                   p->user_name[0] ? p->user_name : "-", p->uid);
        trap_frame_dump(frame);
    }

    printk("\nSystem halted.\n");

    for (;;) {
        cli();
        hlt();
    }
}

void kernel_panic_simple(const char *Message, Nstatus error)
{
    kernel_panic_frame(Message, error, nNULL);
}

bool sysrq_key(char key, const struct trap_frame *frame)
{
    switch (key) {
    case 'c':
        kernel_panic_frame("sysrq: forced crash (SysRq+C)", Npanic, frame);
    case 'h':
        printk("\nsysrq: c = crash (forced kernel panic), h = help\n");
        return true;
    default:
        printk("\nsysrq: unknown key '%c' (h = help)\n", (key >= 0x20 && key < 0x7F) ? key : '?');
        return true;
    }
}
