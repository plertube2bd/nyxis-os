/* stddef.h - C89 7.1.6 */
#ifndef _NL_STDDEF_H
#define _NL_STDDEF_H

#define _NL_WANT_SIZE_T
#define _NL_WANT_WCHAR_T
#define _NL_WANT_NULL
#include "_nl_types.h"

typedef long ptrdiff_t;

#define offsetof(type, member) __builtin_offsetof(type, member)

#endif /* _NL_STDDEF_H */
