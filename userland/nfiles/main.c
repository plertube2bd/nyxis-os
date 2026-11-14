/*
 * nfiles - 파일 탐색기 (NWP 창 프로그램: ntui 의 Super+E, 실행 상자의 "files", 또는 nfiles [디렉터리])
 *
 *   맨 위: 지금 경로.  가운데: 디렉터리 먼저, 이름순 목록 (크기).  맨 아래: 안내 / 입력 / 확인.
 *
 * 이름공간(face:/ tmp:/ app:/ ...)의 뿌리에서 위로 가면 "places" 목록이 나온다 (자주 쓰는 이름공간).
 *
 * 키
 *   ↑ ↓ PgUp PgDn Home End  고르기          Enter / ->  디렉터리 열기, 파일은 kilo 로 편집 (최대화된 창)
 *   Backspace / <-          위 디렉터리     v  보기 (cat, 명령 창)         e  kilo 로 편집
 *   t  여기서 터미널        r  이름 바꾸기   d  지우기 (y 로 확인, 빈 디렉터리만)
 *   n  새 파일              m  새 디렉터리   g  경로로 가기     ~  face:/     .  다시 읽기
 * 입력 중에는 Enter 확정, Esc 취소.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include <spawn.h>
#include <poll.h>
#include <sys/wait.h>
#define NWP_SHORT_NAMES
#include <nwp.h>

/* 창 (NWP): 그리기는 이 창의 칸에 */
static struct nwp_window *g_w;
struct window {
    void *data;
    int wantClose;
};
#define screenPut(x, y, ch, r)          nwp_put_role(g_w, (x), (y), (ch), (r))
#define screenText(x, y, s, r)          nwp_text(g_w, (x), (y), (s), (r))
#define screenFill(x, y, cw, chh, c, r) nwp_fill(g_w, (x), (y), (cw), (chh), (c), (r))

extern char **environ;

/* 창 프로그램을 띄운다 (터미널 nterm). 끝난 것은 이벤트 루프가 거둔다 */
static int spawnArgs(const char *const *argv)
{
    pid_t pid;

    return posix_spawnp(&pid, argv[0], NULL, NULL, (char *const *)argv, environ) == 0 ? 0 : -1;
}

#define FILES_PATH  256
#define FILES_NAME  248
#define FILES_MAX   1024

struct entry {
    char name[FILES_NAME];
    bool isDir;
    long size;
};

enum prompt { P_NONE, P_RENAME, P_MKDIR, P_NEWFILE, P_GOTO, P_DELETE };

struct files {
    char path[FILES_PATH];              /* "" = places 목록 */
    struct entry *ent;
    int count;
    int sel;
    int top;                            /* 목록의 맨 위에 보이는 항목 */
    int rows;                           /* 마지막으로 그린 목록 높이 (PgUp/PgDn) */

    enum prompt prompt;
    char input[FILES_NAME];
    int inLen;
    char msg[96];                       /* 마지막 결과/오류 */
    int reloadSoon;                     /* 편집기를 띄웠다: 다시 활성이 되면 목록을 다시 읽는다 */
};

/* ---------------- 경로 ---------------- */

static bool isNamespaceRoot(const char *p)
{
    size_t n = strlen(p);

    return n >= 2 && p[n - 2] == ':' && p[n - 1] == '/';
}

/* dir + name -> out */
static void joinPath(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlen(dir);

    if (n > 0 && dir[n - 1] == '/')
        snprintf(out, cap, "%s%s", dir, name);
    else
        snprintf(out, cap, "%s/%s", dir, name);
}

/* 위 디렉터리로. 이름공간 뿌리면 places("") 로. 떠난 디렉터리 이름을 child 에 */
static void parentPath(char *path, char *child, size_t cap)
{
    size_t n = strlen(path);
    char *slash;

    child[0] = '\0';
    if (n == 0)
        return;
    if (isNamespaceRoot(path)) {
        snprintf(child, cap, "%s", path);
        path[0] = '\0';
        return;
    }
    if (path[n - 1] == '/')
        path[--n] = '\0';
    slash = strrchr(path, '/');
    if (slash == NULL) {
        path[0] = '\0';
        return;
    }
    snprintf(child, cap, "%s", slash + 1);
    if (slash > path && slash[-1] == ':')
        slash[1] = '\0';                /* "face:/a" -> "face:/" */
    else
        *slash = '\0';
}

/* nysh 명령 줄에 넣을 인자: 작은따옴표로 감싼다 (안의 ' 는 '\'' 로) */
static void shellQuote(char *out, size_t cap, const char *s)
{
    size_t n = 0;

    if (cap < 3)
        return;
    out[n++] = '\'';
    for (; *s && n + 5 < cap; s++) {
        if (*s == '\'') {
            memcpy(out + n, "'\\''", 4);
            n += 4;
        } else {
            out[n++] = *s;
        }
    }
    out[n++] = '\'';
    out[n] = '\0';
}

/* ---------------- 목록 ---------------- */

static int compareEntry(const void *a, const void *b)
{
    const struct entry *x = a;
    const struct entry *y = b;

    if (x->isDir != y->isDir)
        return x->isDir ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void addEntry(struct files *f, const char *name, bool isDir, long size)
{
    if (f->count >= FILES_MAX)
        return;
    snprintf(f->ent[f->count].name, FILES_NAME, "%s", name);
    f->ent[f->count].isDir = isDir;
    f->ent[f->count].size = size;
    f->count++;
}

static void loadPlaces(struct files *f)
{
    char roi[32];

    addEntry(f, "face:/", true, 0);
    addEntry(f, "tmp:/", true, 0);
    addEntry(f, "app:/", true, 0);
    addEntry(f, "disk:/", true, 0);
    addEntry(f, "info:/", true, 0);
    addEntry(f, "dev:/", true, 0);
    snprintf(roi, sizeof(roi), "roi:/%lu/", (unsigned long)getuid());
    addEntry(f, roi, true, 0);
    if (getuid() == 0)
        addEntry(f, "fs:/", true, 0);
}

/* 지금 경로를 다시 읽는다. select 가 있으면 그 이름을 고른다. 못 읽으면 -1 (목록은 비운다) */
static int reload(struct files *f, const char *select)
{
    DIR *d;
    struct dirent *de;
    int i;

    f->count = 0;
    f->sel = 0;
    f->top = 0;
    if (f->path[0] == '\0') {
        loadPlaces(f);
    } else {
        d = opendir(f->path);
        if (d == NULL) {
            snprintf(f->msg, sizeof(f->msg), "%.40s: %s", f->path, strerror(errno));
            return -1;
        }
        while ((de = readdir(d)) != NULL) {
            char full[FILES_PATH + FILES_NAME];
            struct stat st;
            bool isDir = de->d_type == DT_DIR;
            long size = 0;

            joinPath(full, sizeof(full), f->path, de->d_name);
            if (stat(full, &st) == 0) {
                isDir = S_ISDIR(st.st_mode);
                size = (long)st.st_size;
            }
            addEntry(f, de->d_name, isDir, size);
        }
        closedir(d);
        qsort(f->ent, (size_t)f->count, sizeof(f->ent[0]), compareEntry);
    }
    for (i = 0; select != NULL && i < f->count; i++) {
        if (strcmp(f->ent[i].name, select) == 0) {
            f->sel = i;
            break;
        }
    }
    return 0;
}

/* 경로를 바꾼다. 못 열면 원래 경로로 되돌린다 */
static void go(struct files *f, const char *path, const char *select)
{
    char old[FILES_PATH];

    snprintf(old, sizeof(old), "%s", f->path);
    snprintf(f->path, sizeof(f->path), "%s", path);
    if (reload(f, select) < 0) {
        char msg[sizeof(f->msg)];

        memcpy(msg, f->msg, sizeof(msg));
        snprintf(f->path, sizeof(f->path), "%s", old);
        (void)reload(f, NULL);
        memcpy(f->msg, msg, sizeof(msg));
    }
}

static const struct entry *selected(struct files *f)
{
    return (f->sel >= 0 && f->sel < f->count) ? &f->ent[f->sel] : NULL;
}

static void selectedPath(struct files *f, char *out, size_t cap)
{
    const struct entry *e = selected(f);

    if (e == NULL)
        out[0] = '\0';
    else if (f->path[0] == '\0')
        snprintf(out, cap, "%s", e->name);       /* places: 이름이 곧 경로 */
    else
        joinPath(out, cap, f->path, e->name);
}

/* ---------------- 동작 ---------------- */

static void openSelected(struct files *f, bool edit)
{
    const struct entry *e = selected(f);
    char path[FILES_PATH + FILES_NAME];
    char cmd[2 * (FILES_PATH + FILES_NAME) + 16];
    char quoted[sizeof(cmd) - 16];

    if (e == NULL)
        return;
    selectedPath(f, path, sizeof(path));
    if (e->isDir && !edit) {
        go(f, path, NULL);
        return;
    }
    if (e->isDir)
        return;
    shellQuote(quoted, sizeof(quoted), path);
    snprintf(cmd, sizeof(cmd), "kilo %s", quoted);
    {
        const char *argv[] = { "nterm", "-m", "-x", "-t", e->name, "-e", cmd, NULL };

        if (spawnArgs(argv) < 0)
            snprintf(f->msg, sizeof(f->msg), "cannot run nterm");
    }
    f->reloadSoon = 1;                  /* 편집기 창이 닫혀 이 창이 다시 활성이 되면 크기를 다시 읽는다 */
}

static void viewSelected(struct files *f)
{
    const struct entry *e = selected(f);
    char path[FILES_PATH + FILES_NAME];
    char cmd[2 * (FILES_PATH + FILES_NAME) + 16];
    char quoted[sizeof(cmd) - 16];

    if (e == NULL || e->isDir)
        return;
    selectedPath(f, path, sizeof(path));
    shellQuote(quoted, sizeof(quoted), path);
    snprintf(cmd, sizeof(cmd), "cat %s", quoted);
    {
        const char *argv[] = { "nterm", "-t", e->name, "-e", cmd, NULL };

        if (spawnArgs(argv) < 0)
            snprintf(f->msg, sizeof(f->msg), "cannot run nterm");
    }
}

static void startPrompt(struct files *f, enum prompt p, const char *initial)
{
    f->prompt = p;
    snprintf(f->input, sizeof(f->input), "%s", initial);
    f->inLen = (int)strlen(f->input);
    f->msg[0] = '\0';
}

/* 입력을 확정했다 */
static void finishPrompt(struct files *f)
{
    char target[FILES_PATH + FILES_NAME];
    char from[FILES_PATH + FILES_NAME];
    const struct entry *e = selected(f);
    enum prompt p = f->prompt;
    int fd;

    f->prompt = P_NONE;
    f->input[f->inLen] = '\0';
    if (f->inLen == 0)
        return;
    if (p == P_GOTO) {
        go(f, f->input, NULL);
        return;
    }
    if (f->path[0] == '\0') {
        snprintf(f->msg, sizeof(f->msg), "open a directory first");
        return;
    }
    if (strchr(f->input, '/') != NULL && p != P_GOTO) {
        snprintf(f->msg, sizeof(f->msg), "name must not contain '/'");
        return;
    }
    joinPath(target, sizeof(target), f->path, f->input);

    switch (p) {
    case P_RENAME:
        if (e == NULL)
            return;
        joinPath(from, sizeof(from), f->path, e->name);
        if (rename(from, target) != 0)
            snprintf(f->msg, sizeof(f->msg), "rename: %s", strerror(errno));
        break;
    case P_MKDIR:
        if (mkdir(target, 0755) != 0)
            snprintf(f->msg, sizeof(f->msg), "mkdir: %s", strerror(errno));
        break;
    case P_NEWFILE:
        fd = open(target, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0)
            snprintf(f->msg, sizeof(f->msg), "new file: %s", strerror(errno));
        else
            close(fd);
        break;
    default:
        return;
    }
    (void)reload(f, f->input);
}

static void deleteSelected(struct files *f)
{
    const struct entry *e = selected(f);
    char path[FILES_PATH + FILES_NAME];
    int r;

    f->prompt = P_NONE;
    if (e == NULL || f->path[0] == '\0')
        return;
    joinPath(path, sizeof(path), f->path, e->name);
    r = e->isDir ? rmdir(path) : unlink(path);
    if (r != 0)
        snprintf(f->msg, sizeof(f->msg), "delete: %s", strerror(errno));
    else
        snprintf(f->msg, sizeof(f->msg), "deleted %.60s", e->name);
    r = f->sel;
    (void)reload(f, NULL);
    f->sel = r < f->count ? r : f->count - 1;
}

/* ---------------- 앱 함수 ---------------- */

static void sizeText(char *out, size_t cap, const struct entry *e)
{
    if (e->isDir)
        snprintf(out, cap, "<dir>");
    else if (e->size < 1024)
        snprintf(out, cap, "%ldB", e->size);
    else if (e->size < 1024L * 1024L)
        snprintf(out, cap, "%ldK", e->size / 1024);
    else
        snprintf(out, cap, "%ld.%ldM", e->size / (1024L * 1024L), (e->size % (1024L * 1024L)) * 10 / (1024L * 1024L));
}

/* 한 줄을 width 칸에 맞춰 (넘치면 자른다) */
static void textClip(int x, int y, int width, const char *s, int r)
{
    for (int i = 0; i < width && s[i] != '\0'; i++)
        screenPut(x + i, y, (unsigned char)s[i], r);
}

static void filesDraw(struct window *w, int x, int y, int cols, int rows, int active)
{
    struct files *f = w->data;
    int listRows = rows - 2;
    char line[FILES_PATH + 16];

    if (cols < 4 || rows < 3)
        return;
    f->rows = listRows;
    if (active && f->reloadSoon) {
        const struct entry *e = selected(f);
        char name[FILES_NAME];

        f->reloadSoon = 0;
        snprintf(name, sizeof(name), "%s", e != NULL ? e->name : "");
        (void)reload(f, name[0] ? name : NULL);   /* 편집기에서 저장했으면 크기가 바뀌었다 */
    }

    /* 경로 */
    snprintf(line, sizeof(line), " %s", f->path[0] ? f->path : "places");
    screenFill(x, y, cols, 1, ' ', ROLE_TITLE_INACTIVE);
    textClip(x, y, cols, line, ROLE_TITLE_INACTIVE);

    /* 목록: 고른 항목이 보이도록 */
    if (f->sel < f->top)
        f->top = f->sel;
    if (f->sel >= f->top + listRows)
        f->top = f->sel - listRows + 1;
    if (f->top < 0)
        f->top = 0;
    for (int i = 0; i < listRows && f->top + i < f->count; i++) {
        const struct entry *e = &f->ent[f->top + i];
        bool isSel = f->top + i == f->sel;
        int r = isSel ? (active ? ROLE_SELECTION : ROLE_TITLE_INACTIVE) : ROLE_WINDOW;
        char size[16];
        int sizeLen;

        sizeText(size, sizeof(size), e);
        sizeLen = (int)strlen(size);
        if (isSel)
            screenFill(x, y + 1 + i, cols, 1, ' ', r);
        snprintf(line, sizeof(line), " %s%s", e->name,
                 e->isDir && f->path[0] != '\0' ? "/" : "");
        textClip(x, y + 1 + i, cols - sizeLen - 2, line, r);
        if (f->path[0] != '\0' && cols > sizeLen + 12)
            screenText(x + cols - sizeLen - 1, y + 1 + i, size, isSel ? r : ROLE_BORDER);
    }
    if (f->count == 0)
        textClip(x + 1, y + 1, cols - 1, "(empty)", ROLE_BORDER);

    /* 맨 아래: 입력 / 확인 / 결과 / 안내 */
    screenFill(x, y + rows - 1, cols, 1, ' ', ROLE_WINDOW);
    if (f->prompt == P_DELETE) {
        const struct entry *e = selected(f);

        snprintf(line, sizeof(line), "delete %.100s? (y/n)", e ? e->name : "");
        textClip(x, y + rows - 1, cols, line, ROLE_ERROR);
    } else if (f->prompt != P_NONE) {
        static const char *const label[] = { "", "rename to: ", "new directory: ", "new file: ", "go to: " };
        int lx;

        snprintf(line, sizeof(line), "%s%s", label[f->prompt], f->input);
        textClip(x, y + rows - 1, cols - 1, line, ROLE_WINDOW);
        lx = (int)strlen(line);
        if (active && lx < cols)
            screenPut(x + lx, y + rows - 1, ' ', ROLE_SELECTION);
    } else if (f->msg[0] != '\0') {
        textClip(x, y + rows - 1, cols, f->msg, ROLE_ERROR);
    } else {
        textClip(x, y + rows - 1, cols,
                 "Enter open  e edit  v view  t term  r rename  d del  n file  m dir  g go  BS up",
                 ROLE_BORDER);
    }
}

static int promptKey(struct files *f, int key)
{
    if (f->prompt == P_DELETE) {
        if (key == 'y' || key == 'Y')
            deleteSelected(f);
        else
            f->prompt = P_NONE;
        return 1;
    }
    switch (key) {
    case '\r':
    case '\n':
        finishPrompt(f);
        break;
    case KEY_ESC:
    case KEY_CTRL('c'):
        f->prompt = P_NONE;
        break;
    case 0x7F:
    case 0x08:
        if (f->inLen > 0)
            f->input[--f->inLen] = '\0';
        break;
    case KEY_CTRL('u'):
        f->inLen = 0;
        f->input[0] = '\0';
        break;
    default:
        if (key >= 0x20 && key < 0x7F && f->inLen < FILES_NAME - 1) {
            f->input[f->inLen++] = (char)key;
            f->input[f->inLen] = '\0';
        }
    }
    return 1;
}

static int filesKey(struct window *w, int key)
{
    struct files *f = w->data;
    int page = f->rows > 1 ? f->rows - 1 : 1;
    char child[FILES_NAME];
    const struct entry *e;

    if (f->prompt != P_NONE)
        return promptKey(f, key);
    f->msg[0] = '\0';
    e = selected(f);

    switch (key) {
    case KEY_UP:
        if (f->sel > 0)
            f->sel--;
        return 1;
    case KEY_DOWN:
        if (f->sel + 1 < f->count)
            f->sel++;
        return 1;
    case KEY_PGUP:
        f->sel = f->sel > page ? f->sel - page : 0;
        return 1;
    case KEY_PGDN:
        f->sel = f->sel + page < f->count ? f->sel + page : f->count - 1;
        return 1;
    case KEY_HOME:
        f->sel = 0;
        return 1;
    case KEY_END:
        f->sel = f->count - 1;
        return 1;
    case '\r':
    case '\n':
    case KEY_RIGHT:
        openSelected(f, false);
        return 1;
    case 'e':
        openSelected(f, true);
        return 1;
    case 0x7F:
    case 0x08:
    case KEY_LEFT: {
        char path[FILES_PATH];

        snprintf(path, sizeof(path), "%s", f->path);
        parentPath(path, child, sizeof(child));
        if (strcmp(path, f->path) != 0 || f->path[0] != '\0')
            go(f, path, child);
        return 1;
    }
    case 'v':
        viewSelected(f);
        return 1;
    case 't':
        {
            const char *argv[] = { "nterm", f->path[0] ? "-d" : NULL, f->path, NULL };

            if (spawnArgs(argv) < 0)
                snprintf(f->msg, sizeof(f->msg), "cannot run nterm");
        }
        return 1;
    case 'r':
        if (e != NULL && f->path[0] != '\0')
            startPrompt(f, P_RENAME, e->name);
        return 1;
    case 'd':
        if (e != NULL && f->path[0] != '\0')
            f->prompt = P_DELETE;
        return 1;
    case 'n':
        startPrompt(f, P_NEWFILE, "");
        return 1;
    case 'm':
        startPrompt(f, P_MKDIR, "");
        return 1;
    case 'g':
        startPrompt(f, P_GOTO, f->path);
        return 1;
    case '~':
        go(f, "face:/", NULL);
        return 1;
    case '.':
        (void)reload(f, e != NULL ? e->name : NULL);
        return 1;
    }
    return 1;
}

static void filesClose(struct window *w)
{
    struct files *f = w->data;

    free(f->ent);
    free(f);
}

int main(int argc, char **argv)
{
    struct files *f = calloc(1, sizeof(*f));
    struct window win;
    int quit = 0;

    if (f == NULL)
        return 1;
    f->ent = malloc(FILES_MAX * sizeof(f->ent[0]));
    if (f->ent == NULL)
        return 1;
    if (argc > 1)
        snprintf(f->path, sizeof(f->path), "%s", argv[1]);
    else if (getcwd(f->path, sizeof(f->path)) == NULL)
        snprintf(f->path, sizeof(f->path), "face:/");
    if (reload(f, NULL) < 0)
        go(f, "face:/", NULL);

    g_w = nwp_open("nfiles", "files", 0, 0, NWP_STATE_NORMAL);
    if (g_w == NULL) {
        fprintf(stderr, "nfiles: cannot open a window: %s (run it inside ntui)\n", strerror(errno));
        return 1;
    }
    win.data = f;
    win.wantClose = 0;
    while (!quit && !win.wantClose) {
        struct nwp_event ev;
        int r;

        nwp_clear(g_w, ROLE_WINDOW);   /* 창의 모든 칸은 이 프로그램 것: 바탕부터 칠한다 */
        filesDraw(&win, 0, 0, nwp_cols(g_w), nwp_rows(g_w), nwp_focused(g_w));
        (void)nwp_commit(g_w);
        r = nwp_next_event(g_w, &ev, 1000);
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;                           /* 띄운 nterm 을 거둔다 */
        if (r < 0)
            break;                      /* 컴포지터가 없어졌다 */
        if (r == 0)
            continue;
        if (ev.type == NWP_EVENT_KEY)
            (void)filesKey(&win, ev.key);
        else if (ev.type == NWP_EVENT_CLOSE)
            quit = 1;
    }
    filesClose(&win);
    nwp_close(g_w);
    return 0;
}
