/*
 * stdint.h - Nyxis OS 프리스탠딩(freestanding) 환경용 최소 정수형 정의
 *
 * 대상 ABI: x86_64 LP64 (int=32bit, long=64bit, 포인터=64bit).
 *
 * 설계 메모:
 *  - C89 에는 <stdint.h> 와 'long long' 이 없으므로, LP64 에서 64비트인
 *    'long' 을 그대로 사용한다. 이렇게 하면 -std=c89 -pedantic 에서도
 *    'long long' 관련 경고 없이 64비트 정수를 쓸 수 있다.
 *  - 빌드가 LP64 x86_64 가 아니면 즉시 컴파일 오류를 내서, 잘못된 크기의
 *    타입으로 조용히 빌드되는 상황(보안/안정성 위험)을 막는다.
 */
#ifndef STDINT_H
#define STDINT_H

#if !defined(__x86_64__) || !defined(__LP64__)
#error "Nyxis OS requires an x86_64 LP64 compiler (int=32, long=64, pointer=64)."
#endif

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef signed short       int16_t;
typedef unsigned short     uint16_t;
typedef signed int         int32_t;
typedef unsigned int       uint32_t;
typedef signed long        int64_t;
typedef unsigned long      uint64_t;

typedef unsigned long      uintptr_t;
typedef signed long        intptr_t;
typedef unsigned long      size_t;

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

#endif /* STDINT_H */
