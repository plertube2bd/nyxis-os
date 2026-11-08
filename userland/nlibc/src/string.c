/*
 * string.c - C89 7.11 (바이트 단위 단순 구현. 비교는 unsigned char 로)
 */
#include <string.h>
#include <errno.h>

void *memcpy(void *s1, const void *s2, size_t n)
{
    unsigned char *d = (unsigned char *)s1;
    const unsigned char *s = (const unsigned char *)s2;

    while (n--)
        *d++ = *s++;
    return s1;
}

void *memmove(void *s1, const void *s2, size_t n)
{
    unsigned char *d = (unsigned char *)s1;
    const unsigned char *s = (const unsigned char *)s2;

    if (d == s || n == 0)
        return s1;
    if ((unsigned long)d - (unsigned long)s >= n) {   /* d 가 s 앞에 있거나 겹치지 않음: 앞에서부터 */
        while (n--)
            *d++ = *s++;
    } else {                                          /* d 가 [s, s+n) 안: 뒤에서부터 */
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return s1;
}

char *strcpy(char *s1, const char *s2)
{
    char *d = s1;

    while ((*d++ = *s2++) != '\0')
        ;
    return s1;
}

char *strncpy(char *s1, const char *s2, size_t n)
{
    size_t i = 0;

    for (; i < n && s2[i] != '\0'; i++)
        s1[i] = s2[i];
    for (; i < n; i++)
        s1[i] = '\0';
    return s1;
}

char *strcat(char *s1, const char *s2)
{
    strcpy(s1 + strlen(s1), s2);
    return s1;
}

char *strncat(char *s1, const char *s2, size_t n)
{
    char *d = s1 + strlen(s1);

    while (n-- && *s2 != '\0')
        *d++ = *s2++;
    *d = '\0';
    return s1;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;
    size_t i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)a[i] - (int)b[i];
    }
    return 0;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 != '\0' && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

/* "C" 로캘: 대조 순서 == 바이트 순서 */
int strcoll(const char *s1, const char *s2)
{
    return strcmp(s1, s2);
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == '\0')
            return (int)(unsigned char)s1[i] - (int)(unsigned char)s2[i];
    }
    return 0;
}

size_t strxfrm(char *s1, const char *s2, size_t n)
{
    size_t len = strlen(s2);

    if (len < n)
        memcpy(s1, s2, len + 1);
    return len;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char ch = (unsigned char)c;

    for (; n--; p++) {
        if (*p == ch)
            return (void *)p;
    }
    return NULL;
}

char *strchr(const char *s, int c)
{
    char ch = (char)c;

    for (;; s++) {
        if (*s == ch)
            return (char *)s;
        if (*s == '\0')
            return NULL;
    }
}

size_t strcspn(const char *s1, const char *s2)
{
    size_t n = 0;

    while (s1[n] != '\0' && strchr(s2, s1[n]) == NULL)
        n++;
    return n;
}

char *strpbrk(const char *s1, const char *s2)
{
    s1 += strcspn(s1, s2);
    return *s1 != '\0' ? (char *)s1 : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    char ch = (char)c;

    for (;; s++) {
        if (*s == ch)
            last = s;
        if (*s == '\0')
            return (char *)last;
    }
}

size_t strspn(const char *s1, const char *s2)
{
    size_t n = 0;

    while (s1[n] != '\0' && strchr(s2, s1[n]) != NULL)
        n++;
    return n;
}

char *strstr(const char *s1, const char *s2)
{
    size_t n = strlen(s2);

    if (n == 0)
        return (char *)s1;
    for (; *s1 != '\0'; s1++) {
        if (*s1 == *s2 && strncmp(s1, s2, n) == 0)
            return (char *)s1;
    }
    return NULL;
}

char *strtok(char *s1, const char *s2)
{
    static char *next;
    char *tok;

    if (s1 == NULL)
        s1 = next;
    if (s1 == NULL)
        return NULL;
    s1 += strspn(s1, s2);
    if (*s1 == '\0') {
        next = NULL;
        return NULL;
    }
    tok = s1;
    s1 += strcspn(s1, s2);
    if (*s1 != '\0') {
        *s1 = '\0';
        next = s1 + 1;
    } else {
        next = NULL;
    }
    return tok;
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *d = (unsigned char *)s;

    while (n--)
        *d++ = (unsigned char)c;
    return s;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

char *strerror(int errnum)
{
    switch (errnum) {
    case 0:         return "Success";
    case EDOM:      return "Argument out of domain";
    case ERANGE:    return "Result out of range";
    case EPERM:     return "Operation not permitted";
    case ENOENT:    return "No such file or directory";
    case EIO:       return "I/O error";
    case EBADF:     return "Bad handle";
    case EAGAIN:    return "Resource temporarily unavailable";
    case ENOMEM:    return "Out of memory";
    case EACCES:    return "Permission denied";
    case EFAULT:    return "Bad address";
    case EEXIST:    return "File exists";
    case ENOTDIR:   return "Not a directory";
    case EISDIR:    return "Is a directory";
    case EINVAL:    return "Invalid argument";
    case ENOSPC:    return "No space left on device";
    case EROFS:     return "Read-only file system";
    case ENOSYS:    return "Function not implemented";
    case ETIMEDOUT: return "Timed out";
    case ENOTSUP:   return "Not supported";
    case ENOTTY:    return "Not a terminal";
    case EMFILE:    return "Too many open files";
    case ESRCH:     return "No such process";
    case ECHILD:    return "No child processes";
    case ENAMETOOLONG: return "File name too long";
    case ENOTEMPTY: return "Directory not empty";
    case ESPIPE:    return "Illegal seek";
    case E2BIG:     return "Argument list too long";
    case EINTR:     return "Interrupted";
    case EBUSY:     return "Device busy";
    case EPIPE:     return "Broken pipe";
    default:        return "Unknown error";
    }
}
