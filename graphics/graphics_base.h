#ifndef GRAPHICS_BASE_H
#define GRAPHICS_BASE_H

#include "nyxis.h"
#include "library/memory.h"

static inline void draw_pixel(NTBLI* info, i32 x, i32 y, u32 color) {
    u32* fb = (u32*)info->Framebuffer_Base;
    fb[y * info->PixelsPerScanLine + x] = color;
}

static inline void draw_line_plus(NTBLI* info,
    i32 x0, i32 y0,
    i32 x1, i32 y1,
    u32 color
) {
    i32 dx = (x1 > x0) ? x1 - x0 : x0 - x1;
    i32 sx = (x0 < x1) ? 1 : -1;

    i32 dy = (y1 > y0) ? y0 - y1 : y1 - y0;
    i32 sy = (y0 < y1) ? 1 : -1;

    i32 err = dx + dy;

    while (1) {
        draw_pixel(info, x0, y0, color);

        if (x0 == x1 && y0 == y1) break;

        i32 e2 = 2 * err;

        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }

        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static inline void draw_hline_fast(
    NTBLI* info,
    i32 x,
    i32 y,
    i32 length,
    u32 color)
{
    u32* fb = (u32*)info->Framebuffer_Base;
    u32 pitch = info->PixelsPerScanLine;

    if (length <= 0) return;

    u32* dst = fb + y * pitch + x;

    u32 buffer[1024];

    for (i32 i = 0; i < length; i++) {
        buffer[i] = color;
    }

    memcopy(dst, buffer, length * sizeof(u32));
}

#endif // GRAPHICS_BASE_H
