#ifndef INPUTS_BASE_H
#define INPUTS_BASE_H

#include "nyxis.h"

// ==========================
// 포트 I/O
// ==========================
static inline u8 inb(uint16_t port);
static inline void io_wait(void);

// ==========================
// 키보드 상태 (전역 변수)
// ==========================
extern u8 shift_l;
extern u8 shift_r;
extern u8 caps_lock;
extern u8 extended;

// ==========================
// ASCII 테이블
// ==========================
extern utf8 normal[128];
extern utf8 shifted[128];

// ==========================
// 스캔코드 / 입력 함수
// ==========================
u8 get_scancode(void);
char translate(uint8_t sc);
char get_char(void);

#endif // INPUTS_BASE_H
