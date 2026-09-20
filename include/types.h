/*
 * types.h - Nyxis OS 공용 기본 타입 / 에러 코드 / 부트 정보(NTBLI) 정의
 *
 * 부트로더(UEFI 앱)와 커널이 같은 헤더를 공유한다. 따라서 이 헤더에 있는
 * 모든 구조체는 "어떤 파일에서, 어떤 include 순서로 포함하더라도" 항상
 * 같은 크기/오프셋이어야 한다.
 *
 * [수정 이력 요약]
 *  - usize 의 크기를 nyxis.h 의 매크로(NYXIS_64BITS)에 의존하던 문제 수정.
 *    예전에는 types.h 를 nyxis.h 보다 먼저 include 하면 usize 가 32비트가
 *    되어 NTBLI 레이아웃이 부트로더(64비트)와 달라졌다.
 *    (width 필드 오프셋이 56 이어야 하는데 커널에서는 52 로 읽힘)
 *  - C89 호환: _Bool/inline/'//' 주석/enum 끝 쉼표/long long 제거.
 *  - NSTATUS_ERR_FLAG 의 (1 << 31) 는 부호 있는 정수 오버플로(UB) 이므로 수정.
 *  - pack 매크로에서 끝의 ';' 제거, interrupt 매크로(흔한 식별자 오염) 제거.
 *  - NTBLI 에 magic/size/pixel_format/메모리맵 필드를 추가하고 고정폭 타입 사용.
 */
#ifndef NYXIS_TYPES_H
#define NYXIS_TYPES_H

#include <stdint.h>

#if !defined(__x86_64__)
#error "Nyxis OS supports x86_64 only."
#endif

/* 이 OS 는 x86_64 전용이다. (nyxis.h 와 무관하게 항상 정의됨) */
#ifndef NYXIS_64BITS
#define NYXIS_64BITS 1
#endif

/* 구조체 패킹: struct foo { ... } pack; 형태로 사용 */
#define pack __attribute__((packed))

/* 컴파일 타임 assert (C89 용). name 은 전역에서 유일해야 한다. */
#define NX_STATIC_ASSERT(name, cond) \
    typedef char nx_static_assert_##name[(cond) ? 1 : -1] __attribute__((unused))

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

/* C89 에는 _Bool 이 없다. 1바이트 정수로 정의한다. */
typedef unsigned char  bool;
#define true  1
#define false 0

/* 포인터 크기 정수 (x86_64 전용이므로 항상 64비트) */
typedef u64 usize;
typedef i64 isize;

NX_STATIC_ASSERT(usize_is_64bit, sizeof(usize) == 8);
NX_STATIC_ASSERT(ptr_is_64bit, sizeof(void *) == 8);

/* NULL */
#define nNULL ((void *)0)

/* 문자 */
typedef u8  utf8;
typedef u16 utf16;

/* ------------------------------------------------------------------ */
/* 에러 시스템                                                         */
/* ------------------------------------------------------------------ */

/* 상태 코드: 음수(MSB=1)이면 에러 */
typedef i32 Nstatus;

/* MSB = 에러 플래그. (1 << 31) 은 signed overflow 이므로 INT_MIN 을 직접 표기 */
#define NSTATUS_ERR_FLAG  ((i32)(-2147483647 - 1))

#define NSTATUS_OK              0
#define NSTATUS_MAKE_ERR(code)  ((i32)(NSTATUS_ERR_FLAG | (i32)(code)))
#define NSTATUS_IS_ERR(x)       ((x) < 0)
#define NSTATUS_CODE(x)         ((x) & 0x7FFFFFFF)

/* 64개의 에러 코드면 당분간 충분하다. 필요하면 128/256 으로 확장한다. */
typedef enum {
    Nok              = 0,

    /* General */
    NnotFound        = NSTATUS_MAKE_ERR(1),
    NinvalidArg      = NSTATUS_MAKE_ERR(2),
    NoutOfMemory     = NSTATUS_MAKE_ERR(3),
    Npermission      = NSTATUS_MAKE_ERR(4),
    Ntimeout         = NSTATUS_MAKE_ERR(5),
    Nbusy            = NSTATUS_MAKE_ERR(6),
    NalreadyExists   = NSTATUS_MAKE_ERR(7),
    Noverflow        = NSTATUS_MAKE_ERR(8),
    Nunderflow       = NSTATUS_MAKE_ERR(9),
    Nio              = NSTATUS_MAKE_ERR(10),
    Ndisconnected    = NSTATUS_MAKE_ERR(11),
    Nunsupported     = NSTATUS_MAKE_ERR(12),
    Ncorrupted       = NSTATUS_MAKE_ERR(13),
    Ninterrupted     = NSTATUS_MAKE_ERR(14),
    Nunknown         = NSTATUS_MAKE_ERR(15),

    /* Arithmetic */
    NdivideByZero    = NSTATUS_MAKE_ERR(16),
    Narithmetic      = NSTATUS_MAKE_ERR(17),
    NoverflowMath    = NSTATUS_MAKE_ERR(18),
    NunderflowMath   = NSTATUS_MAKE_ERR(19),
    Nnan             = NSTATUS_MAKE_ERR(20),

    /* Memory */
    NnullPointer     = NSTATUS_MAKE_ERR(21),
    NinvalidPointer  = NSTATUS_MAKE_ERR(22),
    NdoubleFree      = NSTATUS_MAKE_ERR(23),
    NuseAfterFree    = NSTATUS_MAKE_ERR(24),
    NstackOverflow   = NSTATUS_MAKE_ERR(25),
    NheapCorruption  = NSTATUS_MAKE_ERR(26),
    NbufferOverflow  = NSTATUS_MAKE_ERR(27),
    NbufferUnderflow = NSTATUS_MAKE_ERR(28),

    /* Filesystem */
    NfileNotFound    = NSTATUS_MAKE_ERR(29),
    NpathTooLong     = NSTATUS_MAKE_ERR(30),
    NreadOnly        = NSTATUS_MAKE_ERR(31),
    NdiskFull        = NSTATUS_MAKE_ERR(32),
    NbadFilesystem   = NSTATUS_MAKE_ERR(33),
    NfileCorrupted   = NSTATUS_MAKE_ERR(34),
    NtooManyFileSystem = NSTATUS_MAKE_ERR(35),

    /* Process / Thread */
    NprocessFailed   = NSTATUS_MAKE_ERR(36),
    NthreadFailed    = NSTATUS_MAKE_ERR(37),
    Ndeadlock        = NSTATUS_MAKE_ERR(38),
    NraceCondition   = NSTATUS_MAKE_ERR(39),

    /* Network */
    NconnectionLost  = NSTATUS_MAKE_ERR(40),
    NconnectionRefused = NSTATUS_MAKE_ERR(41),
    NhostUnreachable = NSTATUS_MAKE_ERR(42),
    NprotocolError   = NSTATUS_MAKE_ERR(43),

    /* Security */
    Nunauthorized    = NSTATUS_MAKE_ERR(44),
    Nauthentication  = NSTATUS_MAKE_ERR(45),
    Nencryption      = NSTATUS_MAKE_ERR(46),

    /* Parsing / Format */
    NinvalidFormat   = NSTATUS_MAKE_ERR(47),
    NparseError      = NSTATUS_MAKE_ERR(48),
    NsyntaxError     = NSTATUS_MAKE_ERR(49),
    NencodingError   = NSTATUS_MAKE_ERR(50),

    /* Device / Hardware */
    NdeviceFailure   = NSTATUS_MAKE_ERR(51),
    NdeviceMissing   = NSTATUS_MAKE_ERR(52),
    NdeviceBusy      = NSTATUS_MAKE_ERR(53),
    NhardwareFault   = NSTATUS_MAKE_ERR(54),

    /* State */
    NnotInitialized  = NSTATUS_MAKE_ERR(55),
    NalreadyInitialized = NSTATUS_MAKE_ERR(56),
    NinvalidState    = NSTATUS_MAKE_ERR(57),

    /* System */
    NsyscallFailed   = NSTATUS_MAKE_ERR(58),
    NkernelFault     = NSTATUS_MAKE_ERR(59),
    Npanic           = NSTATUS_MAKE_ERR(60),
    Nfatal           = NSTATUS_MAKE_ERR(61),

    /* Reserved for future use */
    Nreserved1       = NSTATUS_MAKE_ERR(62),
    Nreserved2       = NSTATUS_MAKE_ERR(63)
} Nerror;

/* ------------------------------------------------------------------ */
/* NTBLI : Nyxis Tiny BootLoader Information                          */
/* 부트로더 -> 커널 전달 구조체 (커널 진입 시 rdi 로 포인터가 전달됨)   */
/* ------------------------------------------------------------------ */

/* 'NTBI' (little endian). 커널은 이 값으로 구조체 유효성을 1차 확인한다. */
#define NTBLI_MAGIC    0x4942544EU
#define NTBLI_VERSION  2U

/* GOP PixelFormat 과 동일한 값 */
#define NTBLI_PIXEL_RGB      0U  /* R,G,B,reserved 각 8비트 */
#define NTBLI_PIXEL_BGR      1U  /* B,G,R,reserved 각 8비트 */
#define NTBLI_PIXEL_BITMASK  2U  /* 비트마스크 (미지원) */
#define NTBLI_PIXEL_BLTONLY  3U  /* 직접 프레임버퍼 접근 불가 */

/*
 * 모든 필드가 고정 폭이며 암묵적 패딩이 없도록 배치되어 있다.
 * (부트로더와 커널이 서로 다른 컴파일 옵션으로 빌드되어도 동일 레이아웃 보장)
 */
typedef struct {
    u32   magic;                 /* NTBLI_MAGIC */
    u32   version;               /* NTBLI_VERSION */
    u32   size;                  /* sizeof(NTBLI) - 커널이 크기 불일치를 검출 */
    u32   reserved0;

    /* 메모리: 사용 가능한 RAM 종류(MMIO 제외) 중 가장 높은 물리 주소의 끝 */
    u64   memory_size;

    /* Initrd (없으면 base=NULL, size=0) */
    void *initrd_base;
    u64   initrd_size;

    /* Framebuffer (없으면 base=NULL) */
    void *framebuffer_base;
    u64   framebuffer_size;
    u32   width;
    u32   height;
    u32   pixels_per_scan_line;
    u32   pixel_format;          /* NTBLI_PIXEL_* */

    /* UEFI 메모리 맵 (EFI_MEMORY_DESCRIPTOR 배열, ExitBootServices 시점의 것) */
    void *memmap_base;
    u64   memmap_size;           /* 바이트 */
    u64   memmap_desc_size;      /* descriptor 1개의 크기 (sizeof 와 다를 수 있음) */
    u32   memmap_desc_version;
    u32   reserved1;

    /* ACPI RSDP (없으면 NULL) */
    void *Rsdp;
} NTBLI;

NX_STATIC_ASSERT(ntbli_size_is_112, sizeof(NTBLI) == 112);

#endif /* NYXIS_TYPES_H */
