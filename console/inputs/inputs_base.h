#ifndef INPUTS_BASE_H
#define INPUTS_BASE_H

#include <Uefi.h>
#include <stdint.h>

// ==========================
// 포트 I/O
// ==========================
static inline uint8_t inb(uint16_t port);
static inline void io_wait(void);

// ==========================
// 키보드 상태 (전역 변수)
// ==========================
extern UINT8 shift_l;
extern UINT8 shift_r;
extern UINT8 caps_lock;
extern UINT8 extended;

// ==========================
// ASCII 테이블
// ==========================
extern char normal[128];
extern char shifted[128];

// ==========================
// 스캔코드 / 입력 함수
// ==========================
uint8_t get_scancode(void);
char translate(uint8_t sc);
char get_char(void);

#endif // INPUTS_BASE_H