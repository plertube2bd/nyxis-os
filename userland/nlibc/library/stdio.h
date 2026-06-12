#ifndef STDIO_H
#define STDIO_H

#include <nyxstd.h>

// 임시 구현
#define NULL ((void*)0)

static inline int strlen(const char *s) {
    int len = 0;
    while (*(s + len)) { len++; }
    return len;
}

#ifdef _NYX64
static inline void puts(const char *s) {
    syscall_wrapper(35, 1, (unsigned long long)s, strlen(s), 0, 0, 0);
}

static inline void printk(const char *s) {
    syscall_wrapper(771, (unsigned long long)s, 0, 0, 0, 0, 0);
}
#else
static inline void puts(const char *s) {
    syscall_wrapper(35, 1, (unsigned int)s, strlen(s), 0, 0, 0);
}

static inline void printk(const char *s) {
    syscall_wrapper(771, (unsigned int)s, 0, 0, 0, 0, 0);
}
#endif

#endif
