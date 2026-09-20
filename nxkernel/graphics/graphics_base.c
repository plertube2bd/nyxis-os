/*
 * graphics_base.c - 프레임버퍼 기본 그리기 (32bpp)
 */

#include "nyxis.h"
#include "memory.h"
#include "graphics/graphics_base.h"

/* info 가 32bpp 직접 접근 가능한 프레임버퍼를 가리키는지 확인 */
static bool fb_usable(const NTBLI *info)
{
    if (!info || !info->framebuffer_base)
        return false;
    if (info->pixel_format != NTBLI_PIXEL_RGB && info->pixel_format != NTBLI_PIXEL_BGR)
        return false;
    if (info->pixels_per_scan_line < info->width)
        return false;

    /* 실제 프레임버퍼 크기 안에 (stride * height * 4) 바이트가 들어가는지 */
    return ((u64)info->pixels_per_scan_line * info->height * 4UL <= info->framebuffer_size)
           ? true : false;
}

void draw_pixel(NTBLI *info, i32 x, i32 y, u32 color)
{
    u32 *fb;

    if (!fb_usable(info))
        return;
    if (x < 0 || y < 0 || (u32)x >= info->width || (u32)y >= info->height)
        return;

    fb = (u32 *)info->framebuffer_base;
    fb[(usize)(u32)y * info->pixels_per_scan_line + (u32)x] = color;
}

static i32 iabs32(i32 v)
{
    return (v < 0) ? -v : v;
}

void draw_line_plus(NTBLI *info, i32 x0, i32 y0, i32 x1, i32 y1, u32 color)
{
    /* 브레젠험: dx >= 0, dy <= 0 로 정의한다 */
    i32 dx = iabs32(x1 - x0);
    i32 sx = (x0 < x1) ? 1 : -1;
    i32 dy = -iabs32(y1 - y0);
    i32 sy = (y0 < y1) ? 1 : -1;
    i32 err = dx + dy;

    for (;;) {
        i32 e2;

        draw_pixel(info, x0, y0, color);

        if (x0 == x1 && y0 == y1)
            break;

        e2 = 2 * err;

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

void draw_hline_fast(NTBLI *info, i32 x, i32 y, i32 length, u32 color)
{
    u32 *row;
    i32 i;
    i32 end;

    if (!fb_usable(info) || length <= 0)
        return;
    if (y < 0 || (u32)y >= info->height)
        return;

    /* 화면 밖 부분 잘라내기 (오버플로 안전) */
    end = x + length;
    if (end < x)                         /* x + length 오버플로 */
        return;
    if (x < 0)
        x = 0;
    if (end > (i32)info->width)
        end = (i32)info->width;
    if (x >= end)
        return;

    row = (u32 *)info->framebuffer_base + (usize)(u32)y * info->pixels_per_scan_line;

    /* 스택 버퍼 없이 직접 채운다 */
    for (i = x; i < end; i++)
        row[i] = color;
}
