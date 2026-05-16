#ifndef _STRING_H
#define _STRING_H

#include "include/types.h"

/* String length */
usize strlen(const utf8* s);

/* String compare */
i32 strcmp(const utf8* a, const utf8* b);
i32 strncmp(const utf8* a, const utf8* b, usize n);

/* String copy */
utf8* strncpy(utf8* dest, const utf8* src, usize n);

/* String concatenation */
utf8* strcat(utf8* dest, const utf8* src);
utf8* strncat(utf8* dest, const utf8* src, usize n);

/* Character search */
utf8* strchr(const utf8* s, i32 c);
utf8* strrchr(const utf8* s, i32 c);

/* Substring search */
utf8* strstr(const utf8* haystack, const utf8* needle);

/* Character set search */
utf8* strpbrk(const utf8* s, const utf8* accept);

/* Tokenizer */
utf8* strtok(utf8* str, const utf8* delim);

#endif
