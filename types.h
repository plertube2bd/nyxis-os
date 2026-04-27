#ifndef NYXIS_TYPES_H
#define NYXIS_TYPES_H

#include <stdint.h>

// fixed size
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

typedef char      i8;
typedef short     i16;
typedef int       i32;
typedef long long i64;


// pointer-sized
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFF
    #define __nyxis_64bits
    typedef u64 usize;
    typedef i64 isize;
#else
    #define __nyxis_32bits
    typedef u32 usize;
    typedef i32 isize;
#endif

// NULL
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
        // C23
    #define nNULL nullptr
#else
    #define nNULL ((void*)0)
#endif

// chars
typedef u8  utf8;
typedef u16 utf16;


// =====================
// error system
// =====================

// status type
typedef i32 Nstatus;

// MSB = error flag
#define NSTATUS_ERR_FLAG ((i32)1 << 31)

// helpers
#define NSTATUS_OK 0
#define NSTATUS_MAKE_ERR(code) (NSTATUS_ERR_FLAG | (code))
#define NSTATUS_IS_ERR(x) ((x) < 0)
#define NSTATUS_CODE(x) ((x) & 0x7FFFFFFF)


// error codes (16개)
typedef enum {
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
    Nfatal           = NSTATUS_MAKE_ERR(16)
} Nerror;

#endif
