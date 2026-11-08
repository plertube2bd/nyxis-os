/*
 * ls [-lad1] [경로...] - 디렉터리 항목을 이름순으로 출력한다 (경로가 없으면 현재 디렉터리)
 *   -l  긴 형식: 종류와 권한, 소유자 uid, gid, 크기, 이름
 *   -a  '.' 로 시작하는 이름도 보인다
 *   -d  디렉터리 자체를 보인다 (안을 나열하지 않는다)
 *   -1  한 줄에 하나 (기본값과 같다)
 * 디렉터리 이름에는 끝에 '/' 를 붙인다. 표준 출력이 터미널이면 색도 쓴다: 디렉터리 굵은 파랑, *.run 초록
 * (NO_COLOR 나 TERM=dumb 이면 끈다).
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "common.h"

static int g_long = 0;
static int g_all = 0;
static int g_dironly = 0;
static int g_color = 0;

static void mode_string(mode_t m, char *out)
{
    static const char rwx[] = "rwxrwxrwx";
    int i;

    out[0] = S_ISDIR(m) ? 'd' : S_ISFIFO(m) ? 'p' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b' : '-';
    for (i = 0; i < 9; i++)
        out[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';
    out[10] = '\0';
}

/* 항목 하나. full 은 stat 할 경로, name 은 보일 이름 */
static void show(const char *full, const char *name, int is_dir)
{
    struct stat st;
    char ms[11];

    if (g_long) {
        if (stat(full, &st) == 0) {
            mode_string(st.st_mode, ms);
            is_dir = S_ISDIR(st.st_mode);
            printf("%s %5lu %5lu %8ld ", ms, (unsigned long)st.st_uid, (unsigned long)st.st_gid, (long)st.st_size);
        } else {
            printf("?????????? %5s %5s %8s ", "?", "?", "?");
        }
    }
    if (g_color) {
        size_t n = strlen(name);
        const char *c = is_dir ? "\033[1;34m"
                      : (n > 4 && strcmp(name + n - 4, ".run") == 0) ? "\033[32m" : NULL;

        if (c) {
            printf("%s%s%s\033[0m\n", c, name, is_dir ? "/" : "");
            return;
        }
    }
    printf("%s%s\n", name, is_dir ? "/" : "");
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int list_dir(const char *path)
{
    char full[CU_PATH_MAX];
    char **names = NULL;
    size_t n = 0;
    size_t cap = 0;
    size_t i;
    struct dirent *e;
    DIR *d = opendir(path);
    int rc = 0;

    if (!d) {
        cu_warn(path);
        return 1;
    }
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' && !g_all)
            continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 32;
            char **nn = (char **)realloc(names, nc * sizeof(*nn));

            if (!nn) {
                rc = 1;
                break;
            }
            names = nn;
            cap = nc;
        }
        /* 이름 뒤에 종류 한 바이트를 붙여 두고 정렬한다 (정렬은 이름만 본다: 종류는 NUL 뒤) */
        names[n] = (char *)malloc(strlen(e->d_name) + 2);
        if (!names[n]) {
            rc = 1;
            break;
        }
        strcpy(names[n], e->d_name);
        names[n][strlen(e->d_name) + 1] = (char)(e->d_type == DT_DIR);
        n++;
    }
    closedir(d);
    if (rc) {
        errno = ENOMEM;
        cu_warn(path);
    }

    qsort(names, n, sizeof(*names), cmp_name);
    for (i = 0; i < n; i++) {
        int is_dir = names[i][strlen(names[i]) + 1];

        if (cu_join(full, sizeof(full), path, names[i]) == 0)
            show(full, names[i], is_dir);
        free(names[i]);
    }
    free(names);
    return rc;
}

int main(int argc, char **argv)
{
    struct stat st;
    int nops;
    int shown = 0;
    int rc = 0;
    int c;
    int i;

    cu_prog = "ls";
    g_color = cu_color_ok(1);
    while ((c = cu_getopt(argc, argv, "lad1")) != -1) {
        if (c == '?')
            return 2;
        if (c == 'l')
            g_long = 1;
        else if (c == 'a')
            g_all = 1;
        else if (c == 'd')
            g_dironly = 1;
    }
    nops = argc - cu_optind;
    if (nops == 0)
        return list_dir(".");

    /* 파일(그리고 -d 의 디렉터리)을 먼저, 그다음 디렉터리마다 내용을 */
    for (i = cu_optind; i < argc; i++) {
        if (stat(argv[i], &st) != 0) {
            cu_warn(argv[i]);
            rc = 1;
            continue;
        }
        if (S_ISDIR(st.st_mode) && !g_dironly)
            continue;
        show(argv[i], argv[i], S_ISDIR(st.st_mode));
        shown = 1;
    }
    if (g_dironly)
        return rc;
    for (i = cu_optind; i < argc; i++) {
        if (stat(argv[i], &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        if (nops > 1) {
            if (shown)
                putchar('\n');
            printf("%s:\n", argv[i]);
            shown = 1;
        }
        rc |= list_dir(argv[i]);
    }
    return rc;
}
