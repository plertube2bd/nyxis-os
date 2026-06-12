#ifndef NYXIS_H
#define NYXIS_H

#ifdef __cplusplus
#error "C++ is not supported. Please compile with a C compiler."
#endif

#include <stdint.h>

#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFF
    #define NYXIS_64BITS
#else
    #define NYXIS_32BITS
#endif

#include "types.h"

// =====================
// Error Checking Macro
// =====================

// Checks if status is an error and returns it if so.
// Usage: NX_ERROR(some_function_call());
#define NX_ERROR(status) do { \
    Nstatus _status = (status); \
    if (NSTATUS_IS_ERR(_status)) { \
        return _status; \
    } \
} while (0)

#include "lowlevel.h"

#endif