/*
 * rename.h - fpdiff 전용: nlibc 의 printf.c / fpconv.c / math.c 를 glibc 와 한 프로그램에 링크하려고
 * 공개 이름을 nl_* 로 바꾼다 (-include 로 맨 앞에 넣는다). errno 는 glibc 의 TLS errno 와 겹치지 않게
 * 일반 변수 nl_errno 로 돌린다.
 */
#include "../../../userland/nlibc/library/errno.h"
#undef errno
extern int nl_errno;
#define errno nl_errno

#define snprintf  nl_snprintf
#define vsnprintf nl_vsnprintf
#define sprintf   nl_sprintf
#define vsprintf  nl_vsprintf
#define printf    nl_printf
#define vprintf   nl_vprintf
#define fprintf   nl_fprintf
#define vfprintf  nl_vfprintf
#define fwrite    nl_fwrite
#define strtod    nl_strtod
#define atof      nl_atof

#define acos  nl_acos
#define asin  nl_asin
#define atan  nl_atan
#define atan2 nl_atan2
#define cos   nl_cos
#define sin   nl_sin
#define tan   nl_tan
#define cosh  nl_cosh
#define sinh  nl_sinh
#define tanh  nl_tanh
#define exp   nl_exp
#define frexp nl_frexp
#define ldexp nl_ldexp
#define log   nl_log
#define log10 nl_log10
#define modf  nl_modf
#define pow   nl_pow
#define sqrt  nl_sqrt
#define ceil  nl_ceil
#define fabs  nl_fabs
#define floor nl_floor
#define fmod  nl_fmod
