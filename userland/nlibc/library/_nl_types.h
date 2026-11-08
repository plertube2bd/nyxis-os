/*
 * _nl_types.h - nlibc 내부: 여러 표준 헤더가 함께 정의해야 하는 타입/매크로 (한 번만 정의되도록 감시)
 *
 * 여러 번 포함된다 (포함하기 전에 _NL_WANT_* 를 정의). 표준 헤더가 아니다. 사용자 코드는 직접 포함하지 말 것.
 * 대상 ABI: x86_64 LP64 (int=32, long=64, 포인터=64). C89 에는 long long 이 없으므로 64비트는 long.
 */

#if !defined(__x86_64__) || !defined(__LP64__)
#error "nlibc requires an x86_64 LP64 compiler"
#endif

#if defined(_NL_WANT_SIZE_T) && !defined(_NL_SIZE_T)
#define _NL_SIZE_T
typedef unsigned long size_t;
#endif

#if defined(_NL_WANT_WCHAR_T) && !defined(_NL_WCHAR_T)
#define _NL_WCHAR_T
typedef int wchar_t;
#endif

#if defined(_NL_WANT_SSIZE_T) && !defined(_NL_SSIZE_T)
#define _NL_SSIZE_T
typedef long ssize_t;
#endif

#if defined(_NL_WANT_OFF_T) && !defined(_NL_OFF_T)
#define _NL_OFF_T
typedef long off_t;
#endif

#if defined(_NL_WANT_TIME_T) && !defined(_NL_TIME_T)
#define _NL_TIME_T
typedef long time_t;
#endif

#if defined(_NL_WANT_PID_T) && !defined(_NL_PID_T)
#define _NL_PID_T
typedef int pid_t;
#endif

#if defined(_NL_WANT_ID_T) && !defined(_NL_ID_T)
#define _NL_ID_T
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int mode_t;
#endif

#if defined(_NL_WANT_NULL) && !defined(NULL)
#define NULL ((void *)0)
#endif

