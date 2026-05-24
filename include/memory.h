#ifndef NYX_MEMORY_H
#define NYX_MEMORY_H

#include "nyxis.h"

/*
========================================
    MEMORY PRIMITIVES
========================================
*/

void *memset(void *dst, int value, usize size);
void *memcpy(void *dst, const void *src, usize size);
void *memmove(void *dst, const void *src, usize size);

#endif // NYX_MEMORY_H