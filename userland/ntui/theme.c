/*
 * theme.c - 색 테마
 *
 * 그리는 코드는 역할(ROLE_*)만 알고 실제 색은 모른다: 색은 여기 표에만 있다.
 * 역할마다 256색 번호와 16색 대체값을 둔다. Mono 는 16색과 속성(반전/굵게)만 쓴다.
 * 설정 theme (nx64_setting_*): 기본값 app:/ntui.run/theme, 고른 테마는 사용자 값 roi:/<uid>/app/ntui/theme 에
 * 이름 한 줄로 저장한다.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <nx64.h>

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

/* 설정 theme 을 불러온다 (사용자 값, 없으면 app:/ntui.run/theme). 없거나 모르는 이름이면 첫 테마(nyx) */
void themeLoad(void)
{
    char name[32];
    const struct theme *t = NULL;

    if (nx64_setting_get("ntui", "theme", name, sizeof(name)) > 0)
        t = themeFind(name);
    currentTheme = t != NULL ? t : &g_themes[0];
}

/* 지금 테마를 사용자 값으로 저장한다. 성공하면 0 */
int themeSave(void)
{
    return nx64_setting_set("ntui", "theme", currentTheme->name);
}
