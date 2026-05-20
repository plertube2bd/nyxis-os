#ifndef STDIO_H
#define STDIO_H

#include <nyxstd.h>

// 임시 구현
#define NULL ((void*)0)

int strlen(const char *s) {
    int len = 0;
    while (*(s + len)) { len++; }
    return len;
}


void puts(const char *s) {
    syscall_wrapper(35, 1, s, strlen(s), NULL, NULL);
}

#endif
