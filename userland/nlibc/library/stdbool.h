/*
 * stdbool.h - bool, true, false (C99 헤더)
 *
 * C99 이상에서는 bool = _Bool (0/1 만 담는 내장 형). C89 로 컴파일하면 _Bool 이 없으므로 unsigned char 로
 * 대신한다 (그때는 0/1 이 아닌 값을 넣으면 그대로 남는다: 조건식으로만 쓴다).
 */
#ifndef _NL_STDBOOL_H
#define _NL_STDBOOL_H

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#define bool  _Bool
#else
#define bool  unsigned char
#endif
#define true  1
#define false 0
#define __bool_true_false_are_defined 1

#endif /* _NL_STDBOOL_H */
