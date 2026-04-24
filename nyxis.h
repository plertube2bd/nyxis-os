#ifndef NYXIS_H
#define NYXIS_H

#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFF
    #define NYXIS_64BITS
#else
    #define NYXIS_32BITS
#endif

#include "types.h"

#endif