/*
 * serial.c - COM1 16550 UART 최소 드라이버 (폴링 방식)
 *
 * 안정성 설계:
 *  - 초기화 때 루프백 테스트로 UART 존재를 확인한다. 없는 하드웨어에 계속 쓰면서
 *    송신 버퍼 비움을 기다리다 멈추는 일을 방지한다.
 *  - 송신 대기는 반복 횟수에 상한을 둔다. (UART 가 응답하지 않아도 커널이 멈추지 않음)
 */

#include "console/outputs/serial.h"
#include "lowlevel.h"

#define COM1_BASE       0x3F8
#define UART_DATA       0   /* DLAB=0: 송수신 / DLAB=1: divisor low */
#define UART_IER        1   /* DLAB=0: 인터럽트 enable / DLAB=1: divisor high */
#define UART_FCR        2
#define UART_LCR        3
#define UART_MCR        4
#define UART_LSR        5

#define LSR_THR_EMPTY   0x20
#define SERIAL_SPIN_MAX 100000UL

static bool g_serial_ready = false;

Nstatus serial_init(void)
{
    u8 probe;

    outb(COM1_BASE + UART_IER, 0x00);   /* 인터럽트 끔 (폴링 사용) */
    outb(COM1_BASE + UART_LCR, 0x80);   /* DLAB 켬 */
    outb(COM1_BASE + UART_DATA, 0x01);  /* divisor = 1 -> 115200 baud */
    outb(COM1_BASE + UART_IER, 0x00);
    outb(COM1_BASE + UART_LCR, 0x03);   /* 8N1, DLAB 끔 */
    outb(COM1_BASE + UART_FCR, 0xC7);   /* FIFO 사용, 클리어, 14바이트 임계 */

    /* 루프백 자가 테스트: 보낸 값이 그대로 돌아와야 UART 가 존재함 */
    outb(COM1_BASE + UART_MCR, 0x1E);
    outb(COM1_BASE + UART_DATA, 0xAE);
    probe = inb(COM1_BASE + UART_DATA);
    if (probe != 0xAE) {
        g_serial_ready = false;
        return NdeviceMissing;
    }

    /* 정상 동작 모드 (OUT1, OUT2, RTS, DTR) */
    outb(COM1_BASE + UART_MCR, 0x0F);
    g_serial_ready = true;
    return NSTATUS_OK;
}

bool serial_available(void)
{
    return g_serial_ready;
}

void serial_putc(char c)
{
    u32 spin;

    if (!g_serial_ready)
        return;

    /* 송신 버퍼가 빌 때까지 (상한 있음) 대기 */
    for (spin = 0; spin < SERIAL_SPIN_MAX; spin++) {
        if (inb(COM1_BASE + UART_LSR) & LSR_THR_EMPTY)
            break;
    }

    outb(COM1_BASE + UART_DATA, (u8)c);
}
