/*
 * stdio.c - C89 7.9 스트림 (printf 계열은 printf.c)
 *
 * 스트림 = 핸들 + 버퍼 하나. 버퍼는 읽기용(buf[pos..len) 이 아직 읽지 않은 데이터) 또는
 * 쓰기용(buf[0..pos) 가 아직 커널에 넘기지 않은 데이터) 중 한 가지로만 쓰인다 (io 필드).
 * 읽기와 쓰기를 바꿀 때 C89 는 사이에 fflush/fseek 를 요구하지만, 여기서는 자동으로 처리한다.
 *
 * 스트림 슬롯은 정적 배열 FOPEN_MAX 개 (0~2 는 stdin/stdout/stderr). 버퍼는 처음 입출력할 때
 * malloc 하고, 실패하면 버퍼 없이 동작한다. stdin/stdout 은 정적 버퍼라 malloc 없이도 쓸 수 있다.
 *
 * 대화형 입력(콘솔 stdin)을 읽기 전에는 줄 단위 버퍼 출력 스트림을 비운다: 프롬프트가 보이도록.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "nl_internal.h"

static struct __nl_file files[FOPEN_MAX];
static unsigned char stdin_buf[BUFSIZ];
static unsigned char stdout_buf[BUFSIZ];

FILE *stdin = &files[0];
FILE *stdout = &files[1];
FILE *stderr = &files[2];

/* fd 의 지금 핸들. 닫힌 fd 면 -1 + EBADF */
static long fh(const FILE *f)
{
    return __nl_fd_to_handle(f->fd);
}

static void init_slot(FILE *f, int fd, int flags)
{
    long h = __nl_fd_to_handle(fd);

    f->fd = fd;
    f->flags = flags | NL_F_INUSE;
    f->bufmode = (h >= 0 && __nl_is_console(h)) ? _IOLBF : _IOFBF;
    f->io = NL_IO_IDLE;
    f->ungot = EOF;
    f->buf = NULL;
    f->bufsize = 0;
    f->pos = 0;
    f->len = 0;
    f->tmpname = NULL;
}

void __nl_stdio_init(void)
{
    init_slot(stdin, 0, NL_F_READ | NL_F_STATIC);
    stdin->buf = stdin_buf;
    stdin->bufsize = sizeof(stdin_buf);

    init_slot(stdout, 1, NL_F_WRITE | NL_F_STATIC);
    stdout->buf = stdout_buf;
    stdout->bufsize = sizeof(stdout_buf);

    init_slot(stderr, 2, NL_F_WRITE | NL_F_STATIC);
    stderr->bufmode = _IONBF;
}

static int valid(FILE *f)
{
    if (f == NULL || f < &files[0] || f >= &files[FOPEN_MAX] || !(f->flags & NL_F_INUSE)) {
        errno = EBADF;
        return 0;
    }
    return 1;
}

static void ensure_buf(FILE *f)
{
    if (f->buf != NULL)
        return;
    if (f->bufmode != _IONBF) {
        f->buf = (unsigned char *)malloc(BUFSIZ);
        if (f->buf != NULL) {
            f->bufsize = BUFSIZ;
            f->flags |= NL_F_MYBUF;
            return;
        }
        f->bufmode = _IONBF;
    }
    f->buf = &f->onebyte;
    f->bufsize = 1;
}

/* 남김없이 쓴다. 실패하면 오류 표시 */
static int write_all(FILE *f, const unsigned char *p, size_t n)
{
    while (n > 0) {
        long h = fh(f);
        long w = h < 0 ? -1 : __nl_write(h, p, n);

        if (w <= 0) {
            f->flags |= NL_F_ERR;
            return EOF;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int flush_write(FILE *f)
{
    int r = 0;

    if (f->io == NL_IO_WRITE) {
        r = write_all(f, f->buf, f->pos);
        f->pos = 0;
        f->io = NL_IO_IDLE;
    }
    return r;
}

/* 읽기 버퍼에 남은 데이터를 버리고 커널의 파일 위치를 논리 위치로 되돌린다 */
static void drop_read(FILE *f)
{
    if (f->io == NL_IO_READ) {
        long unread = (long)(f->len - f->pos) + (f->ungot != EOF ? 1 : 0);

        if (unread)
            (void)lseek(f->fd, -unread, SEEK_CUR);   /* 콘솔처럼 위치가 없는 스트림이면 실패해도 무방 */
    }
    f->io = NL_IO_IDLE;
    f->pos = 0;
    f->len = 0;
    f->ungot = EOF;
}

static void flush_line_buffered(void)
{
    int i;

    for (i = 0; i < FOPEN_MAX; i++) {
        if ((files[i].flags & NL_F_INUSE) && files[i].bufmode == _IOLBF)
            (void)flush_write(&files[i]);
    }
}

/* exit: 모든 스트림을 비우고, tmpfile 은 닫아서 지운다 (C89 7.9.4.3: 프로그램이 끝나면 사라진다) */
static int close_slot(FILE *f);

void __nl_stdio_flush_all(void)
{
    int i;

    (void)fflush(NULL);
    for (i = 0; i < FOPEN_MAX; i++) {
        if ((files[i].flags & NL_F_INUSE) && files[i].tmpname)
            (void)close_slot(&files[i]);
    }
}

/* ---------------- 열기/닫기 ---------------- */
static int parse_mode(const char *mode, int *oflags, int *fflags)
{
    int plus = strchr(mode, '+') != NULL;

    switch (mode[0]) {
    case 'r':
        *oflags = NL_O_READ;
        *fflags = NL_F_READ;
        break;
    case 'w':
        *oflags = NL_O_WRITE | NL_O_CREAT | NL_O_TRUNC;
        *fflags = NL_F_WRITE;
        break;
    case 'a':
        *oflags = NL_O_WRITE | NL_O_CREAT | NL_O_APPEND;   /* 쓰기마다 끝으로: 플랫폼 계층이 맡는다 */
        *fflags = NL_F_WRITE | NL_F_APPEND;
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    if (plus) {
        *oflags |= NL_O_READ | NL_O_WRITE;
        *fflags |= NL_F_READ | NL_F_WRITE;
    }
    return 0;
}

static FILE *open_into(FILE *f, const char *filename, const char *mode, int keep)
{
    int oflags;
    int fflags;
    long h;
    char abs[NL_PATH_MAX];

    if (parse_mode(mode, &oflags, &fflags) < 0)
        return NULL;
    if (__nl_resolve(filename, abs, sizeof(abs)) < 0)
        return NULL;
    h = __nl_open(abs, oflags, 0666);
    if (h < 0)
        return NULL;
    init_slot(f, __nl_handle_to_fd(h), fflags | keep);
    return f;
}

FILE *fopen(const char *filename, const char *mode)
{
    int i;

    for (i = 3; i < FOPEN_MAX; i++) {
        if (!(files[i].flags & NL_F_INUSE))
            return open_into(&files[i], filename, mode, 0);
    }
    errno = EMFILE;
    return NULL;
}

static int close_slot(FILE *f)
{
    int r = flush_write(f);

    if (close(f->fd) < 0)
        r = EOF;
    if (f->flags & NL_F_MYBUF)
        free(f->buf);
    f->buf = NULL;
    f->flags &= NL_F_STATIC;
    if (f->tmpname) {                       /* tmpfile: 닫으면 지운다 */
        (void)__nl_remove(f->tmpname);
        free(f->tmpname);
        f->tmpname = NULL;
    }
    return r;
}

int fclose(FILE *stream)
{
    if (!valid(stream))
        return EOF;
    return close_slot(stream);
}

FILE *freopen(const char *filename, const char *mode, FILE *stream)
{
    int keep;

    if (!valid(stream))
        return NULL;
    keep = stream->flags & NL_F_STATIC;
    (void)close_slot(stream);
    if (open_into(stream, filename, mode, keep) == NULL)
        return NULL;
    /* 정적 슬롯은 정적 버퍼를 되찾는다 */
    if (stream == stdin) {
        stream->buf = stdin_buf;
        stream->bufsize = sizeof(stdin_buf);
    } else if (stream == stdout) {
        stream->buf = stdout_buf;
        stream->bufsize = sizeof(stdout_buf);
    }
    return stream;
}

int fflush(FILE *stream)
{
    int r = 0;
    int i;

    if (stream == NULL) {
        for (i = 0; i < FOPEN_MAX; i++) {
            if ((files[i].flags & NL_F_INUSE) && flush_write(&files[i]) == EOF)
                r = EOF;
        }
        return r;
    }
    if (!valid(stream))
        return EOF;
    if (stream->io == NL_IO_READ) {   /* C89 에서 정의되지 않음: 읽기 버퍼를 버린다 */
        drop_read(stream);
        return 0;
    }
    return flush_write(stream);
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size)
{
    if (!valid(stream) || (mode != _IOFBF && mode != _IOLBF && mode != _IONBF))
        return -1;
    if (stream->io != NL_IO_IDLE || stream->pos != 0)
        return -1;                      /* 입출력을 시작한 뒤에는 바꿀 수 없다 */
    if (stream->flags & NL_F_MYBUF)
        free(stream->buf);
    stream->flags &= ~NL_F_MYBUF;
    stream->bufmode = mode;
    if (mode == _IONBF) {
        stream->buf = &stream->onebyte;
        stream->bufsize = 1;
    } else if (buf != NULL && size > 0) {
        stream->buf = (unsigned char *)buf;
        stream->bufsize = size;
    } else {
        stream->buf = NULL;             /* 처음 쓸 때 malloc */
        stream->bufsize = 0;
    }
    return 0;
}

void setbuf(FILE *stream, char *buf)
{
    (void)setvbuf(stream, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

/* ---------------- 임시 파일 ---------------- */
static int file_exists(const char *path)
{
    long h = __nl_open(path, NL_O_READ, 0);

    if (h < 0)
        return errno != ENOENT;             /* 열 수 없지만 있기는 한 경우(권한)도 "있음" */
    (void)__nl_close(h);
    return 1;
}

/* "<tmpdir>nl<pid>_<번호>" 중 아직 없는 이름. 번호는 호출마다 늘어난다 (TMP_MAX 개까지 서로 다르다) */
char *tmpnam(char *s)
{
    static char internal[L_tmpnam];
    static unsigned int counter;
    char *out = s ? s : internal;
    int tries;

    for (tries = 0; tries < TMP_MAX; tries++) {
        counter = (counter + 1) % TMP_MAX;
        sprintf(out, "%snl%ld_%u", __nl_tmpdir(), __nl_getpid() % 100000000L, counter);
        if (!file_exists(out))
            return out;
    }
    return NULL;
}

FILE *tmpfile(void)
{
    char name[L_tmpnam];
    FILE *f;
    char *copy;

    if (tmpnam(name) == NULL)
        return NULL;
    copy = (char *)malloc(strlen(name) + 1);
    if (copy == NULL)
        return NULL;
    strcpy(copy, name);
    f = fopen(name, "w+b");
    if (f == NULL) {
        free(copy);
        return NULL;
    }
    f->tmpname = copy;
    return f;
}

int remove(const char *filename)
{
    char abs[NL_PATH_MAX];

    if (__nl_resolve(filename, abs, sizeof(abs)) < 0)
        return -1;
    return __nl_remove(abs);
}

int rename(const char *old, const char *new_name)
{
    char a[NL_PATH_MAX];
    char b[NL_PATH_MAX];

    if (__nl_resolve(old, a, sizeof(a)) < 0 || __nl_resolve(new_name, b, sizeof(b)) < 0)
        return -1;
    return __nl_rename(a, b);
}

/* ---------------- POSIX: fd 와 스트림 ---------------- */
int fileno(FILE *stream)
{
    int fd;

    if (!valid(stream))
        return -1;
    fd = stream->fd;
    if (__nl_fd_to_handle(fd) < 0)
        return -1;
    return fd;
}

/* 이미 열린 fd 에 스트림을 씌운다. mode 는 fd 가 허락하는 범위여야 하고, "w" 라도 자르지 않는다 */
FILE *fdopen(int fd, const char *mode)
{
    int oflags;
    int fflags;
    long h = __nl_fd_to_handle(fd);
    int have;
    int i;

    if (h < 0 || parse_mode(mode, &oflags, &fflags) < 0)
        return NULL;
    have = __nl_handle_flags(h);
    if (have >= 0 && (((fflags & NL_F_READ) && !(have & NL_O_READ))
                      || ((fflags & NL_F_WRITE) && !(have & NL_O_WRITE)))) {
        errno = EINVAL;
        return NULL;
    }
    if ((oflags & NL_O_APPEND) && have >= 0)
        (void)__nl_handle_set_append(h, 1);
    for (i = 3; i < FOPEN_MAX; i++) {
        if (!(files[i].flags & NL_F_INUSE)) {
            init_slot(&files[i], fd, fflags);
            return &files[i];
        }
    }
    errno = EMFILE;
    return NULL;
}

/* ---------------- 쓰기 ---------------- */
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    const unsigned char *p = (const unsigned char *)ptr;
    size_t total;
    size_t done = 0;

    if (size == 0 || nmemb == 0)
        return 0;
    if (!valid(stream))
        return 0;
    if (nmemb > (size_t)-1 / size) {
        errno = EINVAL;
        return 0;
    }
    if (!(stream->flags & NL_F_WRITE)) {
        stream->flags |= NL_F_ERR;
        errno = EBADF;
        return 0;
    }
    total = size * nmemb;
    if (stream->io == NL_IO_READ)
        drop_read(stream);
    ensure_buf(stream);

    if (stream->bufmode == _IONBF) {
        if (write_all(stream, p, total) == EOF)
            return 0;
        return nmemb;
    }

    stream->io = NL_IO_WRITE;
    while (done < total) {
        size_t room = stream->bufsize - stream->pos;
        size_t k = total - done;

        if (stream->pos == 0 && k >= stream->bufsize) {     /* 버퍼보다 큰 덩어리는 바로 쓴다 */
            if (write_all(stream, p + done, k) == EOF)
                return done / size;
            done += k;
            break;
        }
        if (k > room)
            k = room;
        memcpy(stream->buf + stream->pos, p + done, k);
        stream->pos += k;
        done += k;
        if (stream->pos == stream->bufsize) {
            if (flush_write(stream) == EOF)
                return (done - stream->pos) / size;
            stream->io = NL_IO_WRITE;
        }
    }
    if (stream->bufmode == _IOLBF && memchr(p, '\n', total) != NULL) {
        if (flush_write(stream) == EOF)
            return 0;
    }
    return nmemb;
}

int fputc(int c, FILE *stream)
{
    unsigned char ch = (unsigned char)c;

    return fwrite(&ch, 1, 1, stream) == 1 ? (int)ch : EOF;
}

int putc(int c, FILE *stream)
{
    return fputc(c, stream);
}

int putchar(int c)
{
    return fputc(c, stdout);
}

int fputs(const char *s, FILE *stream)
{
    size_t n = strlen(s);

    if (n == 0)
        return valid(stream) ? 0 : EOF;
    return fwrite(s, 1, n, stream) == n ? 0 : EOF;
}

int puts(const char *s)
{
    if (fputs(s, stdout) == EOF || fputc('\n', stdout) == EOF)
        return EOF;
    return 0;
}

/* ---------------- 읽기 ---------------- */
/* 버퍼를 다시 채운다. 반환: 0 성공, EOF (끝 또는 오류: 각 표시가 켜진다) */
static int fill(FILE *stream)
{
    long n;

    if (stream->io == NL_IO_WRITE && flush_write(stream) == EOF)
        return EOF;
    ensure_buf(stream);
    if (stream->bufmode != _IOFBF)
        flush_line_buffered();
    n = fh(stream) < 0 ? -1 : __nl_read(fh(stream), stream->buf, stream->bufsize);
    if (n < 0) {
        stream->flags |= NL_F_ERR;
        stream->io = NL_IO_IDLE;
        return EOF;
    }
    if (n == 0) {
        stream->flags |= NL_F_EOF;
        stream->io = NL_IO_IDLE;
        return EOF;
    }
    stream->io = NL_IO_READ;
    stream->pos = 0;
    stream->len = (size_t)n;
    return 0;
}

static int can_read(FILE *stream)
{
    if (!valid(stream))
        return 0;
    if (!(stream->flags & NL_F_READ)) {
        stream->flags |= NL_F_ERR;
        errno = EBADF;
        return 0;
    }
    return 1;
}

int fgetc(FILE *stream)
{
    if (!can_read(stream))
        return EOF;
    if (stream->ungot != EOF) {
        int c = stream->ungot;

        stream->ungot = EOF;
        return c;
    }
    if (stream->flags & NL_F_EOF)       /* 끝 표시는 clearerr 전까지 유지된다 */
        return EOF;
    if (stream->io != NL_IO_READ || stream->pos >= stream->len) {
        if (fill(stream) == EOF)
            return EOF;
    }
    return stream->buf[stream->pos++];
}

int getc(FILE *stream)
{
    return fgetc(stream);
}

int getchar(void)
{
    return fgetc(stdin);
}

int ungetc(int c, FILE *stream)
{
    if (c == EOF || !valid(stream) || stream->ungot != EOF)
        return EOF;
    if (stream->io == NL_IO_WRITE && flush_write(stream) == EOF)
        return EOF;
    if (stream->io == NL_IO_IDLE) {
        stream->io = NL_IO_READ;
        stream->pos = 0;
        stream->len = 0;
    }
    stream->ungot = (unsigned char)c;
    stream->flags &= ~NL_F_EOF;
    return (unsigned char)c;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    unsigned char *p = (unsigned char *)ptr;
    size_t total;
    size_t done = 0;

    if (size == 0 || nmemb == 0)
        return 0;
    if (!can_read(stream))
        return 0;
    if (nmemb > (size_t)-1 / size) {
        errno = EINVAL;
        return 0;
    }
    total = size * nmemb;

    if (stream->ungot != EOF) {
        p[done++] = (unsigned char)stream->ungot;
        stream->ungot = EOF;
    }
    while (done < total && !(stream->flags & NL_F_EOF)) {
        size_t avail;
        size_t k;

        if (stream->io == NL_IO_READ && stream->pos < stream->len) {
            avail = stream->len - stream->pos;
            k = total - done < avail ? total - done : avail;
            memcpy(p + done, stream->buf + stream->pos, k);
            stream->pos += k;
            done += k;
            continue;
        }
        ensure_buf(stream);
        if (total - done >= stream->bufsize && stream->io != NL_IO_WRITE) {
            /* 큰 읽기는 버퍼를 거치지 않는다 */
            long n;

            if (stream->bufmode != _IOFBF)
                flush_line_buffered();
            n = fh(stream) < 0 ? -1 : __nl_read(fh(stream), p + done, total - done);
            if (n < 0) {
                stream->flags |= NL_F_ERR;
                break;
            }
            if (n == 0) {
                stream->flags |= NL_F_EOF;
                break;
            }
            stream->io = NL_IO_IDLE;
            done += (size_t)n;
            continue;
        }
        if (fill(stream) == EOF)
            break;
    }
    return done / size;
}

char *fgets(char *s, int n, FILE *stream)
{
    int i = 0;
    int c;

    if (n <= 0 || !can_read(stream))
        return NULL;
    while (i < n - 1) {
        c = fgetc(stream);
        if (c == EOF) {
            if (i == 0 || (stream->flags & NL_F_ERR))
                return NULL;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    s[i] = '\0';
    return s;
}

char *gets(char *s)
{
    int i = 0;
    int c;

    for (;;) {
        c = fgetc(stdin);
        if (c == EOF) {
            if (i == 0 || ferror(stdin))
                return NULL;
            break;
        }
        if (c == '\n')
            break;
        s[i++] = (char)c;
    }
    s[i] = '\0';
    return s;
}

/* ---------------- 위치 ---------------- */
int fseek(FILE *stream, long offset, int whence)
{
    if (!valid(stream))
        return -1;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        errno = EINVAL;
        return -1;
    }
    if (flush_write(stream) == EOF)
        return -1;
    if (whence == SEEK_CUR && stream->io == NL_IO_READ)
        offset -= (long)(stream->len - stream->pos) + (stream->ungot != EOF ? 1 : 0);
    stream->io = NL_IO_IDLE;
    stream->pos = 0;
    stream->len = 0;
    stream->ungot = EOF;
    if (fh(stream) < 0 || __nl_seek(fh(stream), offset, whence) < 0)
        return -1;
    stream->flags &= ~NL_F_EOF;
    return 0;
}

long ftell(FILE *stream)
{
    long r;

    if (!valid(stream))
        return -1L;
    r = fh(stream) < 0 ? -1 : __nl_seek(fh(stream), 0, SEEK_CUR);
    if (r < 0)
        return -1L;
    if (stream->io == NL_IO_READ)
        r -= (long)(stream->len - stream->pos) + (stream->ungot != EOF ? 1 : 0);
    else if (stream->io == NL_IO_WRITE)
        r += (long)stream->pos;
    return r;
}

void rewind(FILE *stream)
{
    (void)fseek(stream, 0L, SEEK_SET);
    if (valid(stream))
        stream->flags &= ~NL_F_ERR;
}

int fgetpos(FILE *stream, fpos_t *pos)
{
    long r = ftell(stream);

    if (r < 0)
        return -1;
    *pos = r;
    return 0;
}

int fsetpos(FILE *stream, const fpos_t *pos)
{
    return fseek(stream, *pos, SEEK_SET);
}

/* ---------------- 오류 ---------------- */
void clearerr(FILE *stream)
{
    if (valid(stream))
        stream->flags &= ~(NL_F_EOF | NL_F_ERR);
}

int feof(FILE *stream)
{
    return valid(stream) && (stream->flags & NL_F_EOF) != 0;
}

int ferror(FILE *stream)
{
    return valid(stream) && (stream->flags & NL_F_ERR) != 0;
}

void perror(const char *s)
{
    const char *msg = strerror(errno);

    if (s != NULL && *s != '\0')
        fprintf(stderr, "%s: %s\n", s, msg);
    else
        fprintf(stderr, "%s\n", msg);
}

/* ---------------- getline / getdelim (POSIX.1-2008) ---------------- */

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream)
{
    size_t len = 0;
    int c;

    if (!lineptr || !n || !stream) {
        errno = EINVAL;
        return -1;
    }
    if (!*lineptr || *n == 0) {
        *n = 128;
        *lineptr = (char *)realloc(*lineptr, *n);
        if (!*lineptr) {
            errno = ENOMEM;
            return -1;
        }
    }
    for (;;) {
        c = getc(stream);
        if (c == EOF)
            break;
        if (len + 2 > *n) {                 /* 이 글자 + NUL 자리 */
            size_t nn = *n * 2;
            char *p;

            if (nn < *n) {
                errno = ENOMEM;
                return -1;
            }
            p = (char *)realloc(*lineptr, nn);
            if (!p) {
                errno = ENOMEM;
                return -1;
            }
            *lineptr = p;
            *n = nn;
        }
        (*lineptr)[len++] = (char)c;
        if (c == delim)
            break;
    }
    (*lineptr)[len] = '\0';
    if (len == 0)
        return -1;                          /* 아무것도 못 읽음: EOF 또는 오류 (ferror 로 구분) */
    return (ssize_t)len;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
    return getdelim(lineptr, n, '\n', stream);
}
