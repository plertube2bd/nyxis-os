/*
 * tty.c - 콘솔 입력 줄 편집기 + 블로킹 읽기 (설계는 tty.h 참고)
 */

#include "console/inputs/tty.h"
#include "console/inputs/inputs_base.h"
#include "console/outputs/printk.h"
#include "console/outputs/serial.h"
#include "kernel/process/waitq.h"
#include "drivers/pic/pic.h"
#include "interrupt.h"
#include "lowlevel.h"

#define SERIAL_IRQ 4U

static char g_line[TTY_LINE_MAX];          /* 편집 중인 줄 */
static u32 g_line_len = 0;

static char g_ready[TTY_READY_MAX];        /* 완성되어 읽을 수 있는 바이트 (링 버퍼) */
static u32 g_ready_head = 0;               /* 다음에 읽을 위치 */
static u32 g_ready_count = 0;
static u32 g_eof_pending = 0;              /* 아직 소비되지 않은 Ctrl-D(EOF) 수 */

static waitq_t g_readers;

/* 줄을 읽을 수 있는 버퍼로 옮긴다. 공간이 모자라면 줄 전체를 버린다 (줄이 쪼개져 섞이지 않도록) */
static void commit_line(void)
{
    u32 i;

    if (g_line_len > TTY_READY_MAX - g_ready_count) {
        g_line_len = 0;
        return;
    }

    for (i = 0; i < g_line_len; i++)
        g_ready[(g_ready_head + g_ready_count + i) % TTY_READY_MAX] = g_line[i];
    g_ready_count += g_line_len;
    g_line_len = 0;

    (void)waitq_wake_all(&g_readers);
}

void tty_input_char(char c)
{
    u64 flags = irq_save();

    if (c == '\r')
        c = '\n';

    if (c == '\n') {
        g_line[g_line_len++] = '\n';       /* g_line_len <= TTY_LINE_MAX-1 이 항상 유지되므로 자리 있음 */
        printk_write("\n", 1);
        commit_line();
    } else if (c == '\b' || c == 0x7F) {
        if (g_line_len > 0) {
            g_line_len--;
            printk_backspace();
        }
    } else if (c == 0x15) {                /* Ctrl-U */
        while (g_line_len > 0) {
            g_line_len--;
            printk_backspace();
        }
    } else if (c == 0x04) {                /* Ctrl-D */
        if (g_line_len == 0) {
            g_eof_pending++;
            (void)waitq_wake_all(&g_readers);
        } else {
            commit_line();
        }
    } else if (c == '\t' || ((u8)c >= 0x20 && (u8)c < 0x7F)) {
        if (g_line_len < TTY_LINE_MAX - 1U) {
            g_line[g_line_len++] = c;
            printk_write(&c, 1);
        }
    }

    irq_restore(flags);
}

Nstatus tty_read(char *buf, usize len, usize *out_got)
{
    u64 flags;
    usize n = 0;

    if (!buf || !out_got)
        return NinvalidArg;
    *out_got = 0;
    if (len == 0)
        return NSTATUS_OK;

    flags = irq_save();

    while (g_ready_count == 0 && g_eof_pending == 0) {
        Nstatus status = waitq_sleep_locked(&g_readers, 0);

        if (NSTATUS_IS_ERR(status)) {
            irq_restore(flags);
            return status;
        }
    }

    if (g_ready_count > 0) {
        /* 최대 한 줄: '\n' 을 복사하면 멈춘다 */
        while (n < len && g_ready_count > 0) {
            char c = g_ready[g_ready_head];

            buf[n++] = c;
            g_ready_head = (g_ready_head + 1U) % TTY_READY_MAX;
            g_ready_count--;
            if (c == '\n')
                break;
        }
    } else {
        g_eof_pending--;                   /* EOF: 0 바이트 */
    }

    irq_restore(flags);
    *out_got = n;
    return NSTATUS_OK;
}

static void serial_irq_handler(struct trap_frame *frame)
{
    u32 guard = 0;

    (void)frame;
    while (serial_rx_ready() && guard++ < 64U)
        tty_input_char(serial_getc_nowait());
}

void tty_init(void)
{
    waitq_init(&g_readers);

    keyboard_init();

    if (serial_available()) {
        if (irq_register_handler((u8)SERIAL_IRQ, serial_irq_handler) == NSTATUS_OK) {
            serial_enable_rx_interrupt();
            pic_clear_mask((u8)SERIAL_IRQ);
        }
    }
}
