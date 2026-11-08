/*
 * chmod 모드 파일... - 권한 비트를 바꾼다 (소유자나 root 만)
 *   모드: 8진수 (644, 0755) 또는 기호 [ugoa]*[+-=][rwx]*  여럿은 ',' 로 (u+x,go-w)
 *   기호에서 누구를 빼면(+x) umask 에 걸리지 않는 비트만 바꾼다 (POSIX 와 같다).
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "common.h"

/* 기호 모드를 old 에 적용. 형식이 틀리면 -1 */
static int apply_symbolic(const char *s, unsigned old, unsigned mask, unsigned *out)
{
    unsigned mode = old;

    for (;;) {
        unsigned who = 0;
        int any_who = 0;

        for (; *s == 'u' || *s == 'g' || *s == 'o' || *s == 'a'; s++) {
            any_who = 1;
            who |= *s == 'u' ? 0700U : *s == 'g' ? 0070U : *s == 'o' ? 0007U : 0777U;
        }
        if (*s != '+' && *s != '-' && *s != '=')
            return -1;
        while (*s == '+' || *s == '-' || *s == '=') {
            char op = *s++;
            unsigned perm = 0;
            unsigned bits;

            for (; *s == 'r' || *s == 'w' || *s == 'x'; s++)
                perm |= *s == 'r' ? 0444U : *s == 'w' ? 0222U : 0111U;
            bits = perm & (any_who ? who : (0777U & ~mask));
            if (op == '+') {
                mode |= bits;
            } else if (op == '-') {
                mode &= ~bits;
            } else {
                mode &= ~(any_who ? who : 0777U);
                mode |= bits;
            }
        }
        if (*s == '\0')
            break;
        if (*s++ != ',')
            return -1;
    }
    *out = mode & 0777U;
    return 0;
}

int main(int argc, char **argv)
{
    const char *spec;
    unsigned long octal = 0;
    int numeric;
    unsigned mask;
    char *end;
    int rc = 0;
    int i;

    cu_prog = "chmod";
    /* "-w" 같은 기호 모드가 옵션처럼 보이므로 옵션은 받지 않는다 ("--" 만 건너뛴다) */
    i = 1;
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (argc - i < 2) {
        cu_msg("usage: chmod <mode> <file>...");
        return 2;
    }
    spec = argv[i++];
    numeric = spec[0] >= '0' && spec[0] <= '7';
    if (numeric) {
        octal = strtoul(spec, &end, 8);
        if (*end != '\0' || octal > 0777) {
            cu_msg("invalid mode '%s'", spec);
            return 2;
        }
    }
    mask = (unsigned)umask(0);
    (void)umask((mode_t)mask);

    for (; i < argc; i++) {
        struct stat st;
        unsigned mode = (unsigned)octal;

        if (!numeric) {
            if (stat(argv[i], &st) != 0) {
                cu_warn(argv[i]);
                rc = 1;
                continue;
            }
            if (apply_symbolic(spec, (unsigned)st.st_mode & 0777U, mask, &mode) != 0) {
                cu_msg("invalid mode '%s'", spec);
                return 2;
            }
        }
        if (chmod(argv[i], (mode_t)mode) != 0) {
            cu_warn(argv[i]);
            rc = 1;
        }
    }
    return rc;
}
