#ifndef NYXIS_TYPES_H
#define NYXIS_TYPES_H

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
    typedef u64 usize;
    typedef i64 isize;
#else
    typedef u32 usize;
    typedef i32 isize;
#endif

// chars
typedef u8  utf8;
typedef u16 utf16;

#endif