/*
 * serial.h - COM1(0x3F8) 시리얼 콘솔 (디버그/로그 출력용)
 *
 * 그래픽카드가 없거나 프레임버퍼를 못 쓰는 환경에서도 커널 메시지를 볼 수 있게
 * 하는 최소 드라이버이다. QEMU 에서는 -serial stdio / -serial file:... 로 확인.
 */
#ifndef SERIAL_H
#define SERIAL_H

#include "nyxis.h"

/* COM1 을 115200 8N1 로 초기화. UART 가 없으면 NdeviceMissing 을 반환하고 비활성 유지. */
Nstatus serial_init(void);

/* 한 글자 송신. 초기화되지 않았거나 UART 가 없으면 조용히 무시한다. */
void serial_putc(char c);

/* 시리얼이 사용 가능한지 */
bool serial_available(void);

/* 수신 인터럽트(COM1 = IRQ4)를 켠다. 핸들러 등록과 PIC 마스크 해제는 호출자(tty.c)가 한다. */
void serial_enable_rx_interrupt(void);
void serial_disable_rx_interrupt(void);

/* 수신 버퍼에 바이트가 있는지 / 하나 읽기 (serial_rx_ready() 가 true 일 때만 의미 있음) */
bool serial_rx_ready(void);
char serial_getc_nowait(void);

#endif /* SERIAL_H */
