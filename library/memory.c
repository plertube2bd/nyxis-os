#include "nyxis.h"
#include "memory.h"

/*
========================================
    MEMORY PRIMITIVES (WORD OPTIMIZED)
    - memset ( target, value, value_size )
    - memcpy ( target, value, value_size )
    - memmove ( target, value, value_size )
========================================
*/

#define WORD_SIZE sizeof(usize)

/* =========================
 * memset (word optimized)
 * ========================= */
void *memset(void *dst, i8 value, usize size)
{
    utf8 *d8 = (utf8 *)dst;

    // 1. byte fill until aligned
    while (((usize)d8 & (WORD_SIZE - 1)) && size)
    {
        *d8++ = (utf8)value;
        size--;
    }

    // 2. build word pattern
    usize word = 0;
    for (usize i = 0; i < WORD_SIZE; i++)
    {
        word <<= 8;
        word |= (utf8)value;
    }

    // 3. word fill
    usize *dword = (usize *)d8;
    while (size >= WORD_SIZE)
    {
        *dword++ = word;
        size -= WORD_SIZE;
    }

    // 4. tail
    d8 = (utf8 *)dword;
    while (size--)
    {
        *d8++ = (utf8)value;
    }

    return dst;
}

/* =========================
 * memcpy (word optimized)
 * ========================= */
void *memcpy(void *dst, const void *src, usize size)
{
    utf8 *d8 = (utf8 *)dst;
    const utf8 *s8 = (const utf8 *)src;

    // 1. align
    while (((usize)d8 & (WORD_SIZE - 1)) && size)
    {
        *d8++ = *s8++;
        size--;
    }

    // 2. word copy
    usize *dword = (usize *)d8;
    const usize *sword = (const usize *)s8;

    while (size >= WORD_SIZE)
    {
        *dword++ = *sword++;
        size -= WORD_SIZE;
    }

    // 3. tail
    d8 = (utf8 *)dword;
    s8 = (const utf8 *)sword;

    while (size--)
    {
        *d8++ = *s8++;
    }

    return dst;
}

/* =========================
 * memmove (overlap safe)
 * ========================= */
void *memmove(void *dst, const void *src, usize size)
{
    utf8 *d8 = (utf8 *)dst;
    const utf8 *s8 = (const utf8 *)src;

    if (d8 == s8 || size == 0)
        return dst;

    // forward copy
    if (d8 < s8)
    {
        // align
        while (((usize)d8 & (WORD_SIZE - 1)) && size)
        {
            *d8++ = *s8++;
            size--;
        }

        usize *dword = (usize *)d8;
        const usize *sword = (const usize *)s8;

        while (size >= WORD_SIZE)
        {
            *dword++ = *sword++;
            size -= WORD_SIZE;
        }

        d8 = (utf8 *)dword;
        s8 = (const utf8 *)sword;

        while (size--)
        {
            *d8++ = *s8++;
        }
    }
    // backward copy
    else
    {
        d8 += size;
        s8 += size;

        while (((usize)d8 & (WORD_SIZE - 1)) && size)
        {
            *--d8 = *--s8;
            size--;
        }

        usize *dword = (usize *)d8;
        const usize *sword = (const usize *)s8;

        while (size >= WORD_SIZE)
        {
            dword--;
            sword--;
            *dword = *sword;
            size -= WORD_SIZE;
        }

        d8 = (utf8 *)dword;
        s8 = (const utf8 *)sword;

        while (size--)
        {
            *--d8 = *--s8;
        }
    }

    return dst;
}