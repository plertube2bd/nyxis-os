/*
 * nyxis.h - Nyxis OS 공통 헤더 (타입 + 저수준 프리미티브 + 동기화)
 *
 * 커널/드라이버 소스는 대부분 이 헤더 하나만 포함하면 된다.
 * C++ 로 컴파일하는 것은 허용되지 않는다. (C++ 는 유저랜드 전용)
 */
#ifndef NYXIS_H
#define NYXIS_H

#ifdef __cplusplus
#error "C++ is not supported. Please compile with a C compiler."
#endif

#include <stdint.h>
#include "types.h"

/* 구성 일관성 확인: 64비트 x86 이어야 한다. */
#if UINTPTR_MAX != 0xFFFFFFFFFFFFFFFFUL
#error "Nyxis OS supports 64-bit pointers only."
#endif

/* ===================== */
/* Error Checking Macro   */
/* ===================== */

/*
 * status 가 에러이면 그대로 return 한다.
 * 사용 예: NX_ERROR(some_function_call());
 */
#define NX_ERROR(status) do { \
    Nstatus nx_error_status_ = (status); \
    if (NSTATUS_IS_ERR(nx_error_status_)) { \
        return nx_error_status_; \
    } \
} while (0)

#include "lowlevel.h"
#include "sync.h"

#endif /* NYXIS_H */
