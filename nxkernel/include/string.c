#include "include/string.h"
#include "include/memory.h"
#include "include/nyxis.h"

/* strlen */
usize strlen(const utf8* s) {
    usize len = 0;

    while (s[len])
        len++;

    return len;
}

/* strcmp */
i32 strcmp(const utf8* a, const utf8* b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }

    return (u8)*a - (u8)*b;
}

/* strncmp */
i32 strncmp(const utf8* a, const utf8* b, usize n) {
    while (n && *a && (*a == *b)) {
        a++;
        b++;
        n--;
    }

    if (n == 0)
        return 0;

    return (u8)*a - (u8)*b;
}

/* strncpy */
utf8* strncpy(utf8* dest, const utf8* src, usize n) {
    usize i;

    for (i = 0; i < n && src[i]; i++)
        dest[i] = src[i];

    for (; i < n; i++)
        dest[i] = '\0';

    return dest;
}

/* strcat */
utf8* strcat(utf8* dest, const utf8* src) {
    utf8* ptr = dest + strlen(dest);

    while (*src) {
        *ptr++ = *src++;
    }

    *ptr = '\0';

    return dest;
}

/* strncat */
utf8* strncat(utf8* dest, const utf8* src, usize n) {
    utf8* ptr = dest + strlen(dest);

    while (n-- && *src) {
        *ptr++ = *src++;
    }

    *ptr = '\0';

    return dest;
}

/* strchr */
utf8* strchr(const utf8* s, i32 c) {
    while (*s) {
        if (*s == (utf8)c)
            return (utf8*)s;

        s++;
    }

    if ((utf8)c == '\0')
        return (utf8*)s;

    return 0;
}

/* strrchr */
utf8* strrchr(const utf8* s, i32 c) {
    const utf8* last = 0;

    while (*s) {
        if (*s == (utf8)c)
            last = s;

        s++;
    }

    if ((utf8)c == '\0')
        return (utf8)s;

    return (utf8*)last;
}

/* strstr */
utf8* strstr(const utf8* haystack, const utf8* needle) {
    usize needle_len;

    if (!*needle)
        return (utf8*)haystack;

    needle_len = strlen(needle);

    while (*haystack) {
        if (memcmp(haystack, needle, needle_len) == 0)
            return (utf8*)haystack;

        haystack++;
    }

    return 0;
}

/* strpbrk */
char* strpbrk(const utf8* s, const utf8* accept) {
    while (*s) {
        const utf8* a = accept;

        while (*a) {
            if (*s == *a)
                return (utf8*)s;

            a++;
        }

        s++;
    }

    return 0;
}

/* strtok */
utf8* strtok(utf8* str, const utf8* delim) {
    static utf8* next;
    utf8* start;

    if (str)
        next = str;

    if (!next)
        return 0;

    /* Skip delimiters */
    while (*next && strchr(delim, *next))
        next++;

    if (!*next)
        return 0;

    start = next;

    /* Find end of token */
    while (*next && !strchr(delim, *next))
        next++;

    if (*next) {
        *next = '\0';
        next++;
    } else {
        next = 0;
    }

    return start;
}

