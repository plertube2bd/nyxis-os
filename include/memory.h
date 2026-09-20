/*
 * memory.h - 메모리 프리미티브 (freestanding 환경에서 컴파일러도 암묵적으로 호출함)
 */
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
i32 memcmp(const void *a, const void *b, usize size);

#endif /* NYX_MEMORY_H */
