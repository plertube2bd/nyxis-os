/*
 * lock.c - 화면 잠금 (Super+L, Ctrl+Alt+L)
 *
 * 잠그면 창 내용은 그리지 않고 잠금 화면(시계, 사용자, 비밀번호 입력)만 보인다. 그동안 모든 키는 여기로 온다
 * (단축키도 듣지 않는다). 창 안의 프로그램은 계속 돈다 (터미널 출력도 계속 받는다: 다만 보이지 않는다).
 * Enter 를 누르면 커널에 그 사용자의 비밀번호로 인증해 본다 (nx64_check_password = NxAuthenticate: 토큰은 바로 닫는다).
 * 틀리면 커널이 1초 늦춘다 (그동안 ntui 는 멈춘다: 마구 넣어 보기를 늦춘다). Esc 는 입력을 지운다.
 * 비밀번호 버퍼는 확인 뒤 바로 지운다.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <nx64.h>

#include "ntui.h"

#define LOCK_PW_MAX 128

static int g_locked;
static char g_pw[LOCK_PW_MAX];
static struct lineedit g_le;
static char g_msg[64];
static int g_failures;

static void wipe(void)
{
    volatile char *p = g_pw;
    int i;

    for (i = 0; i < LOCK_PW_MAX; i++)
        p[i] = 0;
    leInit(&g_le, g_pw, LOCK_PW_MAX);
}

void lockStart(void)
{
    g_locked = 1;
    g_msg[0] = '\0';
    g_failures = 0;
    wipe();
    menuClose();
}

int lockIsActive(void)
{
    return g_locked;
}

static void centered(int y, const char *s, enum role r)
{
    int x = (screenCols - (int)strlen(s)) / 2;

    screenText(x < 0 ? 0 : x, y, s, r);
}

void lockDraw(void)
{
    char line[96];
    time_t now = time(NULL);
    long m = now == (time_t)-1 ? -1 : (long)((now / 60 + utcOffset) % (24L * 60L));
    int boxW = 40;
    int boxX = (screenCols - boxW) / 2;
    int y = (screenRows - 10) / 2;

    if (!g_locked)
        return;
    screenClear(ROLE_DESKTOP);
    if (m < 0)
        strcpy(line, "--:--");
    else
        snprintf(line, sizeof(line), "%02ld:%02ld", (m + 24L * 60L) % (24L * 60L) / 60, m % 60);
    centered(y, line, ROLE_DESKTOP);
    snprintf(line, sizeof(line), "%s@%s is locked", username, hostname);
    centered(y + 2, line, ROLE_DESKTOP);

    screenFill(boxX, y + 4, boxW, 3, ' ', ROLE_WINDOW);
    screenBox(boxX, y + 4, boxW, 3, ROLE_BORDER);
    screenText(boxX + 2, y + 5, "password:", ROLE_BORDER);
    leDraw(&g_le, boxX + 12, y + 5, boxW - 14, 1, 1);
    if (g_msg[0])
        centered(y + 8, g_msg, ROLE_ERROR);
    else
        centered(y + 8, "Enter unlock  Esc clear", ROLE_DESKTOP);
}

int lockKey(int key)
{
    if (!g_locked)
        return 0;
    if (KEY_BASE(key) == '\r') {
        g_pw[g_le.len] = '\0';
        if (nx64_check_password(g_pw) == 0) {
            g_locked = 0;
            screenInvalidate();
        } else {
            g_failures++;
            snprintf(g_msg, sizeof(g_msg), "wrong password (%d)", g_failures);
        }
        wipe();
        return 1;
    }
    if (KEY_BASE(key) == KEY_ESC) {
        wipe();
        g_msg[0] = '\0';
        return 1;
    }
    (void)leKey(&g_le, key);
    return 1;                           /* 잠겨 있는 동안 다른 곳으로 가는 키는 없다 */
}
