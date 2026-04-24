#ifndef NYX_MEMORY_H
#define NYX_MEMORY_H

#include "nyxis.h"

/*
========================================
    MEMORY PRIMITIVES
========================================
*/

void *memset(void *dst, i8 value, usize size);
void *memcpy(void *dst, const void *src, usize size);
void *memmove(void *dst, const void *src, usize size);

#endif // NYX_MEMORY_H