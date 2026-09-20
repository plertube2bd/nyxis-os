/*
 * string.c - 문자열 함수
 *
 * [수정 이력 요약]
 *  - 인터페이스를 char 기반으로 변경 (string.h 참고).
 *  - strncat: 원래 표준과 동일하게 동작하나, 목적지 버퍼 크기를 모르는
 *    strcpy/strcat 계열은 "버퍼 오버플로의 원인"이 되기 쉬우므로
 *    커널 코드에서는 가급적 strncpy + 명시적 종료 또는 strnlen 을 사용할 것.
 *  - strstr: needle 길이만큼 memcmp 를 하면 haystack 끝을 넘어 읽을 수 있었다.
 *    (haystack 남은 길이가 needle 보다 짧을 때) -> 남은 길이 확인 추가.
 *  - strchr/strrchr/strpbrk 등의 비교는 unsigned char 로 수행.
 */

#include "string.h"
#include "memory.h"
#include "nyxis.h"

/* strlen */
usize strlen(const char *s)
{
    usize len = 0;

    while (s[len])
        len++;

    return len;
}

/* strnlen: 최대 maxlen 까지만 검사 (종료 문자가 없는 버퍼에도 안전) */
usize strnlen(const char *s, usize maxlen)
{
    usize len = 0;

    while (len < maxlen && s[len])
        len++;

    return len;
}

/* strcmp */
i32 strcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) {
        a++;
        b++;
    }

    return (i32)(u8)*a - (i32)(u8)*b;
}

/* strncmp */
i32 strncmp(const char *a, const char *b, usize n)
{
    while (n && *a && (*a == *b)) {
        a++;
        b++;
        n--;
    }

    if (n == 0)
        return 0;

    return (i32)(u8)*a - (i32)(u8)*b;
}

/* strncpy: 표준과 동일 (남는 공간은 0 으로 채움, 꽉 차면 NUL 종료가 없음에 주의) */
char *strncpy(char *dest, const char *src, usize n)
{
    usize i;

    for (i = 0; i < n && src[i]; i++)
        dest[i] = src[i];

    for (; i < n; i++)
        dest[i] = '\0';

    return dest;
}

/* strcpy */
char *strcpy(char *dest, const char *src)
{
    char *out = dest;

    while (*src)
        *out++ = *src++;

    *out = '\0';

    return dest;
}

/* strcat */
char *strcat(char *dest, const char *src)
{
    char *ptr = dest + strlen(dest);

    while (*src)
        *ptr++ = *src++;

    *ptr = '\0';

    return dest;
}

/* strncat: src 에서 최대 n 문자를 붙이고 항상 NUL 종료 */
char *strncat(char *dest, const char *src, usize n)
{
    char *ptr = dest + strlen(dest);

    while (n && *src) {
        *ptr++ = *src++;
        n--;
    }

    *ptr = '\0';

    return dest;
}

/* strchr */
char *strchr(const char *s, i32 c)
{
    while (*s) {
        if ((u8)*s == (u8)c)
            return (char *)s;

        s++;
    }

    if ((u8)c == 0)
        return (char *)s;

    return 0;
}

/* strrchr */
char *strrchr(const char *s, i32 c)
{
    const char *last = 0;

    while (*s) {
        if ((u8)*s == (u8)c)
            last = s;

        s++;
    }

    if ((u8)c == 0)
        return (char *)s;

    return (char *)last;
}

/* strstr */
char *strstr(const char *haystack, const char *needle)
{
    usize needle_len;
    usize hay_len;

    if (!*needle)
        return (char *)haystack;

    needle_len = strlen(needle);
    hay_len = strlen(haystack);

    /* 남은 길이가 needle 보다 짧아지면 더 이상 일치할 수 없다 */
    while (hay_len >= needle_len) {
        if (memcmp(haystack, needle, needle_len) == 0)
            return (char *)haystack;

        haystack++;
        hay_len--;
    }

    return 0;
}

/* strpbrk */
char *strpbrk(const char *s, const char *accept)
{
    while (*s) {
        const char *a = accept;

        while (*a) {
            if (*s == *a)
                return (char *)s;

            a++;
        }

        s++;
    }

    return 0;
}

/* strtok (전역 상태를 사용하므로 재진입/멀티스레드에서 사용 금지) */
char *strtok(char *str, const char *delim)
{
    static char *next;
    char *start;

    if (str)
        next = str;

    if (!next)
        return 0;

    /* 앞쪽 구분자 건너뛰기 */
    while (*next && strchr(delim, (u8)*next))
        next++;

    if (!*next)
        return 0;

    start = next;

    /* 토큰 끝 찾기 */
    while (*next && !strchr(delim, (u8)*next))
        next++;

    if (*next) {
        *next = '\0';
        next++;
    } else {
        next = 0;
    }

    return start;
}
