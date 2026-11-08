/*
 * stdio.h - C89 7.9
 *
 * 스트림은 Nyxis 핸들(include/nyx_abi.h) 위의 버퍼다. stdin/stdout/stderr 는 핸들 0/1/2.
 * 콘솔에 연결된 stdout 은 줄 단위 버퍼, stderr 는 버퍼 없음, 그 밖의 파일은 완전 버퍼.
 *
 * 임시 파일(tmpfile/tmpnam)은 Nyxis 의 tmp:/ (RAM 파일시스템) 에 만든다.
 * snprintf/vsnprintf 와 v*scanf 는 C99 함수지만 쓸모가 커서 함께 제공한다.
 */
#ifndef _NL_STDIO_H
#define _NL_STDIO_H

#define _NL_WANT_SIZE_T
#define _NL_WANT_NULL
#include "_nl_types.h"

typedef struct __nl_file FILE;
typedef long fpos_t;

#define _IOFBF       0
#define _IOLBF       1
#define _IONBF       2

#define BUFSIZ       1024
#define EOF          (-1)
#define FOPEN_MAX    16           /* stdin/stdout/stderr 포함 (커널 핸들은 프로세스당 32개) */
#define FILENAME_MAX 256          /* NX_PATH_MAX */
#define L_tmpnam     32
#define TMP_MAX      10000

#define SEEK_SET     0
#define SEEK_CUR     1
#define SEEK_END     2

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
#define stdin  stdin
#define stdout stdout
#define stderr stderr

/* 7.9.4 파일 연산 */
int remove(const char *filename);
int rename(const char *old, const char *new_name);
FILE *tmpfile(void);
char *tmpnam(char *s);

/* 7.9.5 파일 접근 */
int fclose(FILE *stream);
int fflush(FILE *stream);
FILE *fopen(const char *filename, const char *mode);
FILE *freopen(const char *filename, const char *mode, FILE *stream);
void setbuf(FILE *stream, char *buf);
int setvbuf(FILE *stream, char *buf, int mode, size_t size);

/* 7.9.6 서식 입출력 */
int fprintf(FILE *stream, const char *format, ...);
int printf(const char *format, ...);
int sprintf(char *s, const char *format, ...);
int snprintf(char *s, size_t n, const char *format, ...);
int vfprintf(FILE *stream, const char *format, __builtin_va_list arg);
int vprintf(const char *format, __builtin_va_list arg);
int vsprintf(char *s, const char *format, __builtin_va_list arg);
int vsnprintf(char *s, size_t n, const char *format, __builtin_va_list arg);
int fscanf(FILE *stream, const char *format, ...);
int scanf(const char *format, ...);
int sscanf(const char *s, const char *format, ...);
int vfscanf(FILE *stream, const char *format, __builtin_va_list arg);   /* C99 */
int vscanf(const char *format, __builtin_va_list arg);                 /* C99 */
int vsscanf(const char *s, const char *format, __builtin_va_list arg); /* C99 */

/* 7.9.7 문자 입출력 */
int fgetc(FILE *stream);
char *fgets(char *s, int n, FILE *stream);
int fputc(int c, FILE *stream);
int fputs(const char *s, FILE *stream);
int getc(FILE *stream);
int getchar(void);
char *gets(char *s);                /* C89 에 있으므로 제공하지만 넘침을 막을 수 없다: fgets 를 쓸 것 */
int putc(int c, FILE *stream);
int putchar(int c);
int puts(const char *s);
int ungetc(int c, FILE *stream);

/* 7.9.8 직접 입출력 */
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

/* 7.9.9 위치 */
int fgetpos(FILE *stream, fpos_t *pos);
int fseek(FILE *stream, long offset, int whence);
int fsetpos(FILE *stream, const fpos_t *pos);
long ftell(FILE *stream);
void rewind(FILE *stream);

/* 7.9.10 오류 처리 */
void clearerr(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void perror(const char *s);

#include "_nl_features.h"
#if _NL_POSIX
#define _NL_WANT_SSIZE_T
#include "_nl_types.h"
int fileno(FILE *stream);
FILE *fdopen(int fd, const char *mode);
/* POSIX.1-2008: delim(또는 '\n')까지 한 줄을 *lineptr 에 읽는다 (필요하면 realloc 으로 늘린다).
 * 반환: 읽은 바이트 수(구분자 포함, NUL 제외), EOF 나 오류면 -1 */
ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream);
ssize_t getline(char **lineptr, size_t *n, FILE *stream);
#endif

#endif /* _NL_STDIO_H */
