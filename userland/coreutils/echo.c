/*
 * echo [-neE] [문자열...] - 인자를 공백으로 이어 출력한다
 *
 * 백슬래시 이스케이프는 기본으로 해석한다 (POSIX XSI / dash 와 같다: echo "a\nb" 는 두 줄).
 *   \\  \a  \b  \c(여기서 출력 끝, 줄바꿈도 없음)  \e  \E(ESC)  \f  \n  \r  \t  \v
 *   \0nnn (8진수 최대 3자리)  \xHH (16진수 최대 2자리)
 *   그 밖의 \x 는 글자 그대로 (백슬래시 포함).
 *
 * 옵션 (모든 글자가 n/e/E 인 인자만 옵션이고, 그 밖의 "-x" 는 글자 그대로):
 *   -n  끝의 줄바꿈 없음
 *   -e  이스케이프 해석 (기본값, bash 호환을 위해 받는다)
 *   -E  이스케이프 해석 안 함
 */
#include <stdio.h>
#include <string.h>
#include "common.h"

static int is_option(const char *s)
{
    if (s[0] != '-' || s[1] == '\0')
        return 0;
    for (s++; *s; s++) {
        if (*s != 'n' && *s != 'e' && *s != 'E')
            return 0;
    }
    return 1;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* 이스케이프를 풀어 출력한다. \c 를 만나면 0 (이후 출력 전부 중단) */
static int put_escaped(const char *s)
{
    while (*s) {
        int c = (unsigned char)*s++;
        int v;
        int n;

        if (c != '\\' || *s == '\0') {
            putchar(c);
            continue;
        }

        c = (unsigned char)*s++;
        switch (c) {
        case '\\': putchar('\\'); break;
        case 'a':  putchar('\a'); break;
        case 'b':  putchar('\b'); break;
        case 'c':  return 0;
        case 'e':
        case 'E':  putchar('\033'); break;
        case 'f':  putchar('\f'); break;
        case 'n':  putchar('\n'); break;
        case 'r':  putchar('\r'); break;
        case 't':  putchar('\t'); break;
        case 'v':  putchar('\v'); break;
        case '0':
            v = 0;
            for (n = 0; n < 3 && *s >= '0' && *s <= '7'; n++)
                v = v * 8 + (*s++ - '0');
            putchar(v & 0xFF);
            break;
        case 'x':
            if (hexval((unsigned char)*s) < 0) {
                putchar('\\');
                putchar('x');
                break;
            }
            v = 0;
            for (n = 0; n < 2 && hexval((unsigned char)*s) >= 0; n++)
                v = v * 16 + hexval((unsigned char)*s++);
            putchar(v);
            break;
        default:
            putchar('\\');
            putchar(c);
            break;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    int newline = 1;
    int escapes = 1;
    int i = 1;

    cu_prog = "echo";
    for (; i < argc && is_option(argv[i]); i++) {
        const char *o;

        for (o = argv[i] + 1; *o; o++) {
            if (*o == 'n')
                newline = 0;
            else if (*o == 'e')
                escapes = 1;
            else
                escapes = 0;
        }
    }
    for (; i < argc; i++) {
        if (escapes) {
            if (!put_escaped(argv[i]))
                return (fflush(stdout) == 0) ? 0 : 1;   /* \c: 남은 인자와 줄바꿈 생략 */
        } else {
            fputs(argv[i], stdout);
        }
        if (i + 1 < argc)
            putchar(' ');
    }
    if (newline)
        putchar('\n');
    return (fflush(stdout) == 0) ? 0 : 1;
}
