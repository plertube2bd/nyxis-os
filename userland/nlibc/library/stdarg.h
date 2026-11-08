/* stdarg.h - C89 7.8 (컴파일러 내장 기능) */
#ifndef _NL_STDARG_H
#define _NL_STDARG_H

typedef __builtin_va_list va_list;

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)
#define __va_copy(d, s)    __builtin_va_copy(d, s)

#include "_nl_features.h"
#if _NL_C99
#define va_copy(d, s)      __builtin_va_copy(d, s)
#endif

#endif /* _NL_STDARG_H */
