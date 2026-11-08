/*
 * outputs_base.h - 폰트 데이터(16x16 비트맵 / TrueType 8x16)와 글자 그리기 API
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

/*
 * 콘솔 폰트 준비. CONFIG_FONT_8x16 이면 내장 TrueType 폰트를 8x16 셀로 미리 래스터화해 두고,
 * 실패하거나 CONFIG_FONT_8x16 이 아니면 16x16 비트맵을 쓴다. 여러 번 불러도 한 번만 일한다.
 * 반환값: TTF 를 쓰면 NSTATUS_OK, 비트맵으로 되돌아가면 그 원인 상태 (비트맵 전용 빌드는 NSTATUS_OK).
 */
Nstatus font_init(void);

/* 현재 글자 셀 크기 (픽셀). font_init() 전에는 비트맵 크기 16x16 */
u32 font_cell_width(void);
u32 font_cell_height(void);

/* TrueType 폰트로 그리고 있는가 */
bool font_is_ttf(void);

/* 범위 검사 없음: 호출자가 (x + 셀 폭, y + 셀 높이) 가 화면 안임을 보장해야 함 */
Nstatus draw_char_fast(
    u32 *fb,
    u32  pixels_per_scanline,
    u32  x,
    u32  y,
    utf8 c,
    u32  fg,
    u32  bg
);

/* 범위 검사 있음: 셀이 화면 밖으로 나가면 NinvalidArg */
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
