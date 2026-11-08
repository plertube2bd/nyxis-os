/*
 * stdint.h - 고정폭 정수 (C99 헤더지만 include/nyx_abi.h 가 필요로 하므로 nlibc 가 제공한다)
 *
 * 커널의 include/stdint.h 와 내용이 같되 size_t 를 다른 nlibc 헤더와 겹치지 않게 정의한다.
 * nlibc 를 쓰는 프로그램은 -I userland/nlibc/library 를 include/ 보다 앞에 둔다.
 */
#ifndef _NL_STDINT_H
#define _NL_STDINT_H

#define _NL_WANT_SIZE_T
#include "_nl_types.h"

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef signed short       int16_t;
typedef unsigned short     uint16_t;
typedef signed int         int32_t;
typedef unsigned int       uint32_t;
typedef signed long        int64_t;
typedef unsigned long      uint64_t;
typedef signed long        intptr_t;
typedef unsigned long      uintptr_t;

#define INT8_MAX    127
#define UINT8_MAX   255
#define INT16_MAX   32767
#define UINT16_MAX  65535
#define INT32_MAX   2147483647
#define UINT32_MAX  4294967295U
#define INT64_MAX   0x7FFFFFFFFFFFFFFFL
#define UINT64_MAX  0xFFFFFFFFFFFFFFFFUL
#define INTPTR_MAX  INT64_MAX
#define UINTPTR_MAX UINT64_MAX
#define SIZE_MAX    UINT64_MAX

#define INT8_MIN    (-128)
#define INT16_MIN   (-32767 - 1)
#define INT32_MIN   (-2147483647 - 1)
#define INT64_MIN   (-0x7FFFFFFFFFFFFFFFL - 1)
#define INTPTR_MIN  INT64_MIN
#define PTRDIFF_MIN INT64_MIN
#define PTRDIFF_MAX INT64_MAX

/* 최소 폭/빠른 정수, 최대 폭 정수 */
typedef int8_t   int_least8_t;
typedef uint8_t  uint_least8_t;
typedef int16_t  int_least16_t;
typedef uint16_t uint_least16_t;
typedef int32_t  int_least32_t;
typedef uint32_t uint_least32_t;
typedef int64_t  int_least64_t;
typedef uint64_t uint_least64_t;
typedef int8_t   int_fast8_t;
typedef uint8_t  uint_fast8_t;
typedef int64_t  int_fast16_t;
typedef uint64_t uint_fast16_t;
typedef int64_t  int_fast32_t;
typedef uint64_t uint_fast32_t;
typedef int64_t  int_fast64_t;
typedef uint64_t uint_fast64_t;
typedef int64_t  intmax_t;
typedef uint64_t uintmax_t;
#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX

#define INT8_C(c)   c
#define UINT8_C(c)  c
#define INT16_C(c)  c
#define UINT16_C(c) c
#define INT32_C(c)  c
#define UINT32_C(c) c ## U
#define INT64_C(c)  c ## L
#define UINT64_C(c) c ## UL
#define INTMAX_C(c)  c ## L
#define UINTMAX_C(c) c ## UL

#endif /* _NL_STDINT_H */
