#include "nyxis.h"
#include "outputs_base.h"

// 커서 상태 (전역)
static u32 g_cursor_x = 0;
static u32 g_cursor_y = 0;

// 화면 설정 (필요시 외부에서 설정)
static u32 g_screen_width = 1024;
static u32 g_screen_height = 768;

#define FONT_W 16
#define FONT_H 16

void printk(const char *string)
{
    while (*string)
    {
        char c = *string++;

        // newline 처리
        if (c == '\n') {
            g_cursor_x = 0;
            g_cursor_y += FONT_H;
            continue;
        }

        // 화면 끝 처리 (wrap)
        if (g_cursor_x + FONT_W >= g_screen_width) {
            g_cursor_x = 0;
            g_cursor_y += FONT_H;
        }

        // 화면 아래 넘으면 스크롤 or clamp (지금은 clamp)
        if (g_cursor_y + FONT_H >= g_screen_height) {
            g_cursor_y = 0; // 또는 scroll 구현 가능
        }

        DrawCharFast(
            fb,
            pixels_per_scanline,
            g_cursor_x,
            g_cursor_y,
            (utf8)c,
            0xFFFFFF, // fg (white)
            0x000000  // bg (black)
        );

        g_cursor_x += FONT_W;
    }
}