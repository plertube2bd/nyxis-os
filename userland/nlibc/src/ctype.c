/*
 * ctype.c - C89 7.3 ("C" 로캘, 7비트 ASCII. EOF 와 128~255 는 어느 분류에도 속하지 않는다)
 */
#include <ctype.h>

int isdigit(int c)  { return c >= '0' && c <= '9'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int isalpha(int c)  { return islower(c) || isupper(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int iscntrl(int c)  { return (c >= 0 && c < 0x20) || c == 0x7F; }
int isgraph(int c)  { return c > 0x20 && c < 0x7F; }
int isprint(int c)  { return c >= 0x20 && c < 0x7F; }
int ispunct(int c)  { return isgraph(c) && !isalnum(c); }
int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int tolower(int c)  { return isupper(c) ? c - 'A' + 'a' : c; }
int toupper(int c)  { return islower(c) ? c - 'a' + 'A' : c; }
