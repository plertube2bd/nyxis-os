/* sys/types.h - POSIX 기본 타입 (x86_64 LP64) */
#ifndef _NL_SYS_TYPES_H
#define _NL_SYS_TYPES_H

#define _NL_WANT_SIZE_T
#define _NL_WANT_SSIZE_T
#define _NL_WANT_OFF_T
#define _NL_WANT_PID_T
#define _NL_WANT_ID_T
#define _NL_WANT_TIME_T
#include "../_nl_types.h"

typedef unsigned long ino_t;
typedef unsigned long dev_t;
typedef unsigned long nlink_t;

#endif /* _NL_SYS_TYPES_H */
