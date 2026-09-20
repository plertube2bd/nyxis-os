/*
 * string.h - 문자열 함수
 *
 * [수정 이력 요약]
 *  - 인자/반환 타입을 utf8(unsigned char) 에서 표준 C 와 같은 char 로 변경.
 *    호출하는 쪽은 거의 모두 char* 이므로, utf8* 이면 -Wpointer-sign 경고가
 *    나거나 형변환이 필요했다. (바이트 단위 비교는 구현에서 unsigned char 로 수행)
 *  - strnlen 추가: 신뢰할 수 없는 버퍼에서 문자열 길이를 안전하게 잴 때 사용.
 */
#ifndef _STRING_H
#define _STRING_H

#include "types.h"

/* String length */
usize strlen(const char *s);
usize strnlen(const char *s, usize maxlen);

/* String compare */
i32 strcmp(const char *a, const char *b);
i32 strncmp(const char *a, const char *b, usize n);

/* String copy (strncpy 는 표준과 같이 n 바이트를 0 으로 채운다) */
char *strncpy(char *dest, const char *src, usize n);
char *strcpy(char *dest, const char *src);

/* String concatenation */
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, usize n);

/* Character search */
char *strchr(const char *s, i32 c);
char *strrchr(const char *s, i32 c);

/* Substring search */
char *strstr(const char *haystack, const char *needle);

/* Character set search */
char *strpbrk(const char *s, const char *accept);

/* Tokenizer (비재진입: 전역 상태 사용) */
char *strtok(char *str, const char *delim);

#endif /* _STRING_H */
