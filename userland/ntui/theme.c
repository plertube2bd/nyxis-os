/*
 * theme.c - 색 테마
 *
 * 그리는 코드는 역할(ROLE_*)만 알고 실제 색은 모른다: 색은 여기 표에만 있다.
 * 역할마다 256색 번호와 16색 대체값을 둔다. Mono 는 16색과 속성(반전/굵게)만 쓴다.
 * 고른 테마는 roi:/<uid>/app/ntui/theme 에 이름 한 줄로 저장한다.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "ntui.h"

/* style = { fg, bg, fg16, bg16, attr } */

static const struct theme g_themes[] = {
    /* Nyx: 거의 검정에 가까운 바탕 + 깊은 보라/청록 강조의 어두운 기본 테마 */
    { "nyx", 0, {
        {  60, 232,  8,  0, 0 },                        /* ROLE_DESKTOP        #080808 */
        { 250, 234,  7,  0, 0 },                        /* ROLE_WINDOW         #1c1c1c */
        {  60, 234,  8,  0, 0 },                        /* ROLE_BORDER         #5f5f87 */
        { 231,  54, 15,  5, ATTR_BOLD },                /* ROLE_TITLE_ACTIVE   #5f0087 */
        { 244, 235,  8,  0, 0 },                        /* ROLE_TITLE_INACTIVE #262626 */
        { 231,  23, 15,  6, 0 },                        /* ROLE_SELECTION      #005f5f */
        { 248, 233,  7,  0, 0 },                        /* ROLE_TASKBAR        #121212 */
        { 231,  88, 15,  1, ATTR_BOLD }                 /* ROLE_ERROR          #870000 */
    } },

    /* Daylight: 흰 창, 연회색 바탕화면, 파란 강조 */
    { "daylight", 0, {
        { 238, 250,  0,  7, 0 },                        /* ROLE_DESKTOP */
        {  16, 231,  0, 15, 0 },                        /* ROLE_WINDOW */
        { 244, 231,  8, 15, 0 },                        /* ROLE_BORDER */
        { 231,  26, 15,  4, ATTR_BOLD },                /* ROLE_TITLE_ACTIVE */
        { 244, 253,  8,  7, 0 },                        /* ROLE_TITLE_INACTIVE */
        { 231,  25, 15, 12, 0 },                        /* ROLE_SELECTION */
        {  16, 254,  0,  7, 0 },                        /* ROLE_TASKBAR */
        { 231, 160, 15,  1, ATTR_BOLD }                 /* ROLE_ERROR */
    } },

    /* Mono: 기본 8색 + 반전/굵게만. 시리얼 터미널, 고대비, "색 없이도 구분되는지" 검사용 */
    { "mono", 1, {
        {   7,   0,  7,  0, 0 },                        /* ROLE_DESKTOP */
        {   7,   0,  7,  0, 0 },                        /* ROLE_WINDOW */
        {   7,   0,  7,  0, ATTR_BOLD },                /* ROLE_BORDER */
        {   7,   0,  7,  0, ATTR_REVERSE | ATTR_BOLD }, /* ROLE_TITLE_ACTIVE */
        {   7,   0,  7,  0, 0 },                        /* ROLE_TITLE_INACTIVE */
        {   7,   0,  7,  0, ATTR_REVERSE },             /* ROLE_SELECTION */
        {   7,   0,  7,  0, ATTR_BOLD },                /* ROLE_TASKBAR (반전이면 활성 창 반전과 구분되지 않는다) */
        {   7,   0,  7,  0, ATTR_REVERSE | ATTR_BOLD }  /* ROLE_ERROR */
    } }
};

#define THEME_COUNT (sizeof(g_themes) / sizeof(g_themes[0]))

const struct theme *currentTheme = &g_themes[0];

const struct theme *themeFind(const char *name)
{
    size_t i;

    for (i = 0; i < THEME_COUNT; i++) {
        if (strcmp(g_themes[i].name, name) == 0)
            return &g_themes[i];
    }
    return NULL;
}

void themeNext(void)
{
    size_t i = (size_t)(currentTheme - g_themes);

    currentTheme = &g_themes[(i + 1) % THEME_COUNT];
}

/* roi:/<uid>/app/ntui (dir != 0) 또는 그 안의 theme 파일 경로 */
static void themePath(char *buf, size_t cap, int dir)
{
    snprintf(buf, cap, dir ? "roi:/%lu/app/ntui" : "roi:/%lu/app/ntui/theme", (unsigned long)uid);
}

/* 저장된 테마를 불러온다 (init 이후: uid 가 필요). 없거나 모르는 이름이면 기본(nyx) */
void themeLoad(void)
{
    char path[64];
    char name[32];
    const struct theme *t = NULL;
    FILE *f;

    themePath(path, sizeof(path), 0);
    f = fopen(path, "r");
    if (f != NULL) {
        if (fgets(name, sizeof(name), f) != NULL) {
            name[strcspn(name, "\n")] = '\0';
            t = themeFind(name);
        }
        fclose(f);
    }
    currentTheme = t != NULL ? t : &g_themes[0];
}

/* 지금 테마를 저장한다. 성공하면 0 */
int themeSave(void)
{
    char path[64];
    FILE *f;

    themePath(path, sizeof(path), 1);
    if (mkdir(path, 0700) < 0 && errno != EEXIST)
        return -1;
    themePath(path, sizeof(path), 0);
    f = fopen(path, "w");
    if (f == NULL)
        return -1;
    fprintf(f, "%s\n", currentTheme->name);
    return fclose(f) == 0 ? 0 : -1;
}

/*
 * 역할 r 의 SGR 시퀀스("\033[0;...m")를 buf 에 만든다. colors: 터미널이 쓸 수 있는 색 수 (256 또는 16).
 * Mono 처럼 only16 인 테마는 언제나 16색 값을 쓴다. 돌려주는 값은 길이 (잘렸으면 cap 이상).
 */
int themeSgr(enum role r, int colors, char *buf, size_t cap)
{
    const struct style *s = &currentTheme->role[r];
    char attr[8];
    int n;

    attr[0] = '\0';
    if (s->attr & ATTR_BOLD)
        strcat(attr, ";1");
    if (s->attr & ATTR_REVERSE)
        strcat(attr, ";7");

    if (colors >= 256 && !currentTheme->only16) {
        n = snprintf(buf, cap, "\033[0%s;38;5;%u;48;5;%um", attr, (unsigned)s->fg, (unsigned)s->bg);
    } else {
        /* 0-7 -> 30-37 / 40-47, 8-15 -> 90-97 / 100-107 */
        unsigned fg = s->fg16 < 8 ? 30U + s->fg16 : 90U + (s->fg16 - 8U);
        unsigned bg = s->bg16 < 8 ? 40U + s->bg16 : 100U + (s->bg16 - 8U);

        n = snprintf(buf, cap, "\033[0%s;%u;%um", attr, fg, bg);
    }
    return n;
}
