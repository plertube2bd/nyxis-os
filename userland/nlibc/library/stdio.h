/*
 * stdio.h - Nyxis 유저랜드 최소 입출력 (시스템 콜 래퍼)
 */
#ifndef STDIO_H
#define STDIO_H

#include <nyxstd.h>

/* NULL 정의 */
#define NULL ((void *)0)

static __inline__ unsigned long strlen(const char *s)
{
    unsigned long len = 0;
    while (*(s + len)) { len++; }
    return len;
}

/* NxWrite(fd=1, buf, len) - 커널에 아직 구현되지 않음 (NsyscallFailed 반환) */
static __inline__ void puts(const char *s)
{
    syscall_wrapper(35, 1, (nyx_u64)(unsigned long)s, strlen(s), 0, 0, 0);
}

/* NxKernelPrint(str) */
static __inline__ void printk(const char *s)
{
    syscall_wrapper(771, (nyx_u64)(unsigned long)s, 0, 0, 0, 0, 0);
}

#endif /* STDIO_H */
