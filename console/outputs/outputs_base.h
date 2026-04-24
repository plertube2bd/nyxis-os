#ifndef BASE_H
#define BASE_H

// =====================
// Font Info
// =====================
#define FONT_WIDTH            16
#define FONT_HEIGHT           16
#define FONT_BYTES_PER_CHAR   32

#define FONT_FIRST_CHAR       32   // ASCII ' '
#define FONT_LAST_CHAR        126  // ASCII '~'
#define FONT_CHAR_COUNT       (FONT_LAST_CHAR - FONT_FIRST_CHAR + 1)

// =====================
// Font Data
// =====================
extern const UINT8 FONTS[FONT_CHAR_COUNT][FONT_BYTES_PER_CHAR];

// =====================
// API
// =====================
const UINT8* GetFont(char c);

#endif