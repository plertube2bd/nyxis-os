#ifndef PRINTK_H
#define PRINTK_H

#include "nyxis.h"

static u32 g_cursor_x;
static u32 g_cursor_y;
static u32 g_screen_width;
static u32 g_screen_height;

#define FONT_W 16
#define FONT_H 16

void printk(const char *string);

#endif