/*
 * stdlib.h - C89 7.10
 */
#ifndef _NL_STDLIB_H
#define _NL_STDLIB_H

#define _NL_WANT_SIZE_T
#define _NL_WANT_WCHAR_T
#define _NL_WANT_NULL
#include "_nl_types.h"

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;

#define EXIT_FAILURE 1
#define EXIT_SUCCESS 0
#define RAND_MAX     2147483647
#define MB_CUR_MAX   1            /* "C" 로캘만 지원 */

/* 7.10.1 수 변환 */
double atof(const char *nptr);
int atoi(const char *nptr);
long atol(const char *nptr);
double strtod(const char *nptr, char **endptr);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);

/* 7.10.2 의사 난수 */
int rand(void);
void srand(unsigned int seed);

/* 7.10.3 메모리 관리 (NxVirtualAlloc/brk 위의 할당자) */
void *calloc(size_t nmemb, size_t size);
void free(void *ptr);
void *malloc(size_t size);
void *realloc(void *ptr, size_t size);

/* 7.10.4 환경 */
#ifdef __GNUC__
#define _NL_NORETURN __attribute__((__noreturn__))
#else
#define _NL_NORETURN
#endif
void abort(void) _NL_NORETURN;
int atexit(void (*func)(void));
void exit(int status) _NL_NORETURN;
char *getenv(const char *name);
int system(const char *string);     /* 명령 처리기 없음: system(NULL) == 0, 그 밖은 -1 */

/* 7.10.5 검색과 정렬 */
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));

/* 7.10.6 정수 산술 */
int abs(int j);
div_t div(int numer, int denom);
long labs(long j);
ldiv_t ldiv(long numer, long denom);

/* 7.10.7~8 멀티바이트 ("C" 로캘: 바이트 하나 = 문자 하나) */
int mblen(const char *s, size_t n);
int mbtowc(wchar_t *pwc, const char *s, size_t n);
int wctomb(char *s, wchar_t wchar);
size_t mbstowcs(wchar_t *pwcs, const char *s, size_t n);
size_t wcstombs(char *s, const wchar_t *pwcs, size_t n);

#include "_nl_features.h"
#if _NL_C99
/* C99: long long (= long, 64비트) 와 float/long double 변환. strtold 는 double 정밀도로 읽는다 */
__extension__ long long strtoll(const char *nptr, char **endptr, int base);
__extension__ unsigned long long strtoull(const char *nptr, char **endptr, int base);
__extension__ long long atoll(const char *nptr);
__extension__ long long llabs(long long j);
float strtof(const char *nptr, char **endptr);
long double strtold(const char *nptr, char **endptr);
void _Exit(int status) _NL_NORETURN;
#endif
#if _NL_POSIX
/* 경로를 절대 경로로 정리한다 ("." ".." 처리, 이름공간 접두어 포함). 없는 경로면 ENOENT. 결과가 NULL 이면 malloc */
char *realpath(const char *path, char *resolved);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
#endif

#endif /* _NL_STDLIB_H */
