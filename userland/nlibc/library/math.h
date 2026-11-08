/*
 * math.h - C89 7.5
 *
 * 오류 처리 (C89 7.5.1): 정의역 오류면 errno = EDOM 이고 NaN 을 돌려준다.
 * 결과가 넘치면 errno = ERANGE 이고 ±HUGE_VAL, 아래로 넘쳐 0 이 되면 errno = ERANGE 이고 0.
 */
#ifndef _NL_MATH_H
#define _NL_MATH_H

#define HUGE_VAL (__builtin_huge_val())

/* 7.5.2 삼각 함수 */
double acos(double x);
double asin(double x);
double atan(double x);
double atan2(double y, double x);
double cos(double x);
double sin(double x);
double tan(double x);

/* 7.5.3 쌍곡선 함수 */
double cosh(double x);
double sinh(double x);
double tanh(double x);

/* 7.5.4 지수/로그 */
double exp(double x);
double frexp(double value, int *exp);
double ldexp(double x, int exp);
double log(double x);
double log10(double x);
double modf(double value, double *iptr);

/* 7.5.5 거듭제곱 */
double pow(double x, double y);
double sqrt(double x);

/* 7.5.6 올림/내림, 절댓값, 나머지 */
double ceil(double x);
double fabs(double x);
double floor(double x);
double fmod(double x, double y);

#include "_nl_features.h"
#if _NL_C99
long double ldexpl(long double x, int exp);     /* x87 fscale: 80비트 정밀도 그대로 */
long double fabsl(long double x);
#endif

#endif /* _NL_MATH_H */
