/*
 * outputs_base.h - 폰트 데이터와 글자 그리기 API
 */
#ifndef BASE_H
#define BASE_H

#include "nyxis.h"

/* ===================== */
/* Font Info              */
/* ===================== */
#define FONT_WIDTH            16
#define FONT_HEIGHT           16
#define FONT_BYTES_PER_CHAR   32

#define FONT_FIRST_CHAR       32   /* ASCII ' ' */
#define FONT_LAST_CHAR        126  /* ASCII '~' */
#define FONT_CHAR_COUNT       (FONT_LAST_CHAR - FONT_FIRST_CHAR + 1)

/* ===================== */
/* Font Data              */
/* ===================== */
extern const u8 FONTS[FONT_CHAR_COUNT][FONT_BYTES_PER_CHAR];

/* ===================== */
/* API                    */
/* ===================== */
const u8 *GetFont(char c);

/* 범위 검사 없음: 호출자가 경계를 보장해야 함 */
Nstatus draw_char_fast(
    u32 *fb,
    u32  pixels_per_scanline,
    u32  x,
    u32  y,
    utf8 c,
    u32  fg,
    u32  bg
);

/* 범위 검사 있음: 화면 밖이면 NinvalidArg */
Nstatus draw_char_bounded(
    u32 *fb,
    u32  pixels_per_scanline,
    u32  width,
    u32  height,
    u32  x,
    u32  y,
    utf8 c,
    u32  fg,
    u32  bg
);

#endif /* BASE_H */
