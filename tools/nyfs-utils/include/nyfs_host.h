/*
 * nyfs_host.h - 리눅스 호스트 툴(mkfs.nyfs, nyfs-fuse) 공용 기본 타입/에러 코드
 *
 * NyxisOS 커널의 include/types.h 와 "값/의미"는 동일하게 맞추되, 이 헤더는
 * 프리스탠딩이 아니라 일반 리눅스 유저스페이스(glibc) 위에서 빌드된다는 점만
 * 다르다. 고정폭 타입은 <stdint.h>(C89 환경에서도 gcc/glibc 가 제공)를 쓰고,
 * C89 에는 _Bool/stdbool.h 가 없으므로 커널 쪽과 똑같이 unsigned char 기반
 * bool 을 직접 정의한다.
 *
 * 이 파일 자체는 -std=c89 -pedantic -Wall -Wextra -Werror 로 경고 없이
 * 컴파일되는 것을 목표로 한다(빌드 규칙: docs/BUILD_RULES.md 또는 저장소
 * 최상위 지침 참고). FUSE3 헤더(fuse3/fuse.h)는 C99 API 이므로 nyfs_fuse.c
 * 파일 하나만 예외적으로 -std=gnu99 로 빌드한다 - 그 사실과 이유는
 * nyfs_fuse.c 상단 주석에도 다시 적어 둔다.
 */
#ifndef NYFS_HOST_H
#define NYFS_HOST_H

#include <stdint.h>
#include <stddef.h> /* size_t */

/* 구조체 패킹: struct foo { ... } pack; 형태로 사용한다(커널 쪽과 동일 관례) */
#define pack __attribute__((packed))

/* 컴파일 타임 assert (C89 용) - 이름은 전역에서 유일해야 한다 */
#define NYFS_STATIC_ASSERT(name, cond) \
    typedef char nyfs_static_assert_##name[(cond) ? 1 : -1] __attribute__((unused))

/* ------------------------------------------------------------------ */
/* 고정 폭 정수                                                        */
/* ------------------------------------------------------------------ */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef uint64_t       u64;

typedef signed char    i8;
typedef short          i16;
typedef int            i32;
typedef int64_t        i64;

/* C89 에는 _Bool 이 없다. 커널 코드와 똑같이 1바이트 정수로 정의한다. */
typedef unsigned char  nbool;
#define ntrue  1
#define nfalse 0

typedef size_t usize;

#define nNULL ((void *)0)

NYFS_STATIC_ASSERT(u64_is_8_bytes, sizeof(u64) == 8);
NYFS_STATIC_ASSERT(u32_is_4_bytes, sizeof(u32) == 4);

/* ------------------------------------------------------------------ */
/* 상태 코드 - 커널 include/types.h 의 Nstatus/Nerror 와 값 의미를 맞춘        */
/* 부분집합. 디스크에 저장되는 값이 아니라 이 툴 내부에서만 쓰는 반환값이라   */
/* 커널과 비트 단위로 ABI 를 맞출 필요는 없고, 이름과 "무엇을 뜻하는지"만    */
/* 맞춰서 코드를 읽는 사람이 커널 쪽 nyfs.c 와 바로 대응시킬 수 있게 한다.   */
/* ------------------------------------------------------------------ */
typedef enum {
    Nok               = 0,
    NnotFound         = -1,
    NinvalidArg       = -2,
    NoutOfMemory      = -3,
    Npermission       = -4,
    Nbusy             = -6,
    NalreadyExists    = -7,
    Nio               = -10,
    Nunsupported      = -12,
    Ncorrupted        = -13,
    NreadOnly         = -31,
    NdiskFull         = -32,
    NbadFilesystem    = -33,
    NnameTooLong      = -34,
    Noverflow         = -8
} nyfs_status_t;

#define NSTATUS_IS_ERR(x) ((x) != Nok)

#endif /* NYFS_HOST_H */
