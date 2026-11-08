/*
 * _nl_features.h - nlibc 내부: C 표준 헤더 안의 POSIX 확장을 보일지 정한다
 *
 * glibc 와 같은 규칙: 엄격한 표준 모드(-std=c89 등, __STRICT_ANSI__)에서는 C 표준 이름만 보이고,
 * _POSIX_C_SOURCE / _XOPEN_SOURCE / _NYX_SOURCE 를 정의하면 POSIX 확장(fileno, kill, setenv,
 * clock_gettime ...)도 보인다. -std=gnu89 처럼 엄격하지 않으면 항상 보인다.
 * unistd.h 처럼 POSIX 에만 있는 헤더는 이 규칙과 상관없이 모든 선언을 보인다.
 */
#ifndef _NL_FEATURES_H
#define _NL_FEATURES_H

#if !defined(__STRICT_ANSI__) || defined(_POSIX_C_SOURCE) || defined(_XOPEN_SOURCE) || defined(_NYX_SOURCE)
#define _NL_POSIX 1
#else
#define _NL_POSIX 0
#endif

/*
 * C99 확장 (long long 함수, va_copy, strtof/strtold, ldexpl ...): -std=c99 이상이거나, POSIX 확장이 보일 때
 * (POSIX.1-2001 은 C99 를 포함한다). long long 은 __extension__ 으로 선언해 -std=c89 -pedantic 에서도 경고가 없다.
 */
#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L) || _NL_POSIX
#define _NL_C99 1
#else
#define _NL_C99 0
#endif

#endif /* _NL_FEATURES_H */
