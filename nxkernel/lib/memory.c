/*
 * memory.c - memset / memcpy / memmove / memcmp
 *
 * freestanding 환경에서는 컴파일러가 구조체 복사 등을 위해 이 함수들을
 * 암묵적으로 호출한다. 따라서 이 함수들은 (1) 절대 자기 자신을 호출하면 안 되고
 * (2) 어떤 상황에서도 정확해야 한다.
 *
 * [수정 이력 요약]
 *  - C89 호환: for 루프 내 선언, '//' 주석 제거.
 *  - 워드 단위 복사 시 정렬 검사를 dst 만 하던 것을 dst/src 모두 정렬됐을 때만
 *    워드 복사를 하도록 변경. (예전에는 정렬되지 않은 src 를 usize* 로 역참조:
 *    표준상 UB 이며, 다른 아키텍처/최적화에서 오동작 가능)
 *  - 워드 접근은 may_alias 타입으로 수행 (strict aliasing 위반 방지).
 *  - memmove 의 방향 판단을 "dst 가 src 범위 안에 있는지" 로 정확히 수정.
 *    (기존 d8 < s8 비교만으로는 겹치지 않는 경우에도 역방향으로 복사했음: 정확성은
 *    같지만 불필요) 이제 겹침이 없거나 dst < src 이면 정방향 복사.
 *
 * 주의: 최적화(-O2)에서 GCC 가 아래 루프를 memset/memcpy 호출로 바꿔 버리면
 *       무한 재귀가 된다. Makefile 에서 -fno-tree-loop-distribute-patterns 를 지정한다.
 */

#include "nyxis.h"
#include "memory.h"

/* 워드 단위 접근용 타입: 별칭(alias) 허용 */
typedef usize __attribute__((__may_alias__)) nx_word_t;

#define NX_WORD_SIZE  ((usize)sizeof(nx_word_t))
#define NX_WORD_MASK  (NX_WORD_SIZE - 1)

/* =========================
 * memset
 * ========================= */
void *memset(void *dst, int value, usize size)
{
    u8 *d8 = (u8 *)dst;
    u8 byte = (u8)value;
    usize word = 0;
    usize i;

    /* 1. 정렬될 때까지 바이트 단위로 채운다 */
    while (size && ((usize)d8 & NX_WORD_MASK)) {
        *d8++ = byte;
        size--;
    }

    /* 2. 워드 패턴 생성 */
    for (i = 0; i < NX_WORD_SIZE; i++) {
        word <<= 8;
        word |= byte;
    }

    /* 3. 워드 단위 채우기 */
    while (size >= NX_WORD_SIZE) {
        *(nx_word_t *)d8 = word;
        d8 += NX_WORD_SIZE;
        size -= NX_WORD_SIZE;
    }

    /* 4. 나머지 */
    while (size--)
        *d8++ = byte;

    return dst;
}

/* =========================
 * memcpy (겹치지 않는 영역 전용)
 * ========================= */
void *memcpy(void *dst, const void *src, usize size)
{
    u8 *d8 = (u8 *)dst;
    const u8 *s8 = (const u8 *)src;

    /* dst/src 가 모두 워드 정렬이면 워드 단위로 복사 */
    if ((((usize)d8 | (usize)s8) & NX_WORD_MASK) == 0) {
        while (size >= NX_WORD_SIZE) {
            *(nx_word_t *)d8 = *(const nx_word_t *)s8;
            d8 += NX_WORD_SIZE;
            s8 += NX_WORD_SIZE;
            size -= NX_WORD_SIZE;
        }
    }

    while (size--)
        *d8++ = *s8++;

    return dst;
}

/* =========================
 * memmove (겹침 허용)
 * ========================= */
void *memmove(void *dst, const void *src, usize size)
{
    u8 *d8 = (u8 *)dst;
    const u8 *s8 = (const u8 *)src;

    if (d8 == s8 || size == 0)
        return dst;

    /*
     * dst 가 src 앞에 있거나 dst 가 src 범위 밖이면 정방향 복사가 안전하다.
     * (포인터 비교는 정수로 바꿔서 수행: 서로 다른 객체 간 비교의 UB 회피)
     */
    if ((usize)d8 < (usize)s8 || (usize)d8 >= (usize)s8 + size)
        return memcpy(dst, src, size);

    /* dst 가 src 범위 안쪽(뒤쪽)에 걸치는 경우: 역방향 복사 */
    d8 += size;
    s8 += size;

    if ((((usize)d8 | (usize)s8) & NX_WORD_MASK) == 0) {
        while (size >= NX_WORD_SIZE) {
            d8 -= NX_WORD_SIZE;
            s8 -= NX_WORD_SIZE;
            *(nx_word_t *)d8 = *(const nx_word_t *)s8;
            size -= NX_WORD_SIZE;
        }
    }

    while (size--)
        *--d8 = *--s8;

    return dst;
}

/* =========================
 * memcmp
 * ========================= */
i32 memcmp(const void *a, const void *b, usize size)
{
    const u8 *s1 = (const u8 *)a;
    const u8 *s2 = (const u8 *)b;

    while (size--) {
        if (*s1 != *s2)
            return (i32)*s1 - (i32)*s2;
        s1++;
        s2++;
    }

    return 0;
}
