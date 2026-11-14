/*
 * keymap_test.c - ntui 단축키 설정(userland/ntui/keymap.c) 호스트 검사 (make test-ntui)
 *
 * 설정 읽기/쓰기와 화면/창 상태는 아래 가짜 함수로 바꾼다: 기본값은 rootfs/app/ntui/keys 를 그대로 읽는다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <nx64.h>

#include "keymap.h"

static int g_fail;
static int g_count;
static const char *g_defaultPath;
static char g_user[16384];
static int g_haveUser;
static int g_display = 1;
static const char *g_app = "";

/* ---- 가짜 플랫폼 ---- */
long nyx_setting_get_from(int where, const char *app, const char *key, char *buf, size_t cap)
{
    FILE *f;
    size_t n;

    (void)app;
    (void)key;
    if (where == NYX_SETTING_USER) {
        if (!g_haveUser) {
            errno = ENOENT;
            return -1;
        }
        snprintf(buf, cap, "%s", g_user);
        return (long)strlen(buf);
    }
    f = fopen(g_defaultPath, "r");
    if (f == NULL)
        return -1;
    n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}
int nx64_setting_set(const char *app, const char *key, const char *value)
{
    (void)app;
    (void)key;
    snprintf(g_user, sizeof(g_user), "%s\n", value);
    g_haveUser = 1;
    return 0;
}
int nx64_setting_reset(const char *app, const char *key)
{
    (void)app;
    (void)key;
    g_haveUser = 0;
    return 0;
}

static void check(int ok, const char *what)
{
    g_count++;
    if (!ok) {
        g_fail++;
        printf("FAIL %s\n", what);
    }
}

static int key(const char *s)
{
    int k = -1;

    return kmParseKey(s, &k) == 0 ? k : -1;
}

static int firstAct(int k)
{
    const struct km_binding *b = kmLookup(k, g_app, g_display);

    return b ? kmStmtAction(&b->s[0]) : -1;
}

int main(int argc, char **argv)
{
    char err[200];
    char buf[256];
    struct km_binding b;
    int n;
    int k;

    g_defaultPath = argc > 1 ? argv[1] : "rootfs/app/ntui/keys";

    /* 키 이름과 정규화 */
    check(key("super+r") == ('r' | KEY_MOD_SUPER), "super+r");
    check(key("Win+R") == ('r' | KEY_MOD_SUPER), "win+R is case-insensitive");
    check(key("ctrl+alt+t") == ('t' | KEY_MOD_CTRL | KEY_MOD_ALT), "ctrl+alt+t");
    check(key("shift+1") == '!', "shift+1 = !");
    check(key("super+shift+m") == ('m' | KEY_MOD_SUPER | KEY_MOD_SHIFT), "super+shift+m");
    check(key("f11") == KEY_F11 && key("alt+f4") == (KEY_F4 | KEY_MOD_ALT), "f keys");
    check(key("super") == KEY_SUPER_TAP && key("ctrl+super") == -1, "super alone only");
    check(key("ctrl+plus") == ('+' | KEY_MOD_CTRL), "plus name");
    check(key("ctrl+") == -1 && key("nope") == -1 && key("a+b") == -1, "bad keys rejected");
    /* 들어오는 키 -> 정규 키 */
    check(kmCanon(KEY_CTRL('t') | KEY_MOD_ALT) == key("ctrl+alt+t"), "canon: Ctrl+Alt+T arrives as 0x14|Alt");
    check(kmCanon('M' | KEY_MOD_SUPER) == key("super+shift+m"), "canon: uppercase = shift");
    check(kmCanon(KEY_CTRL('w') | KEY_MOD_SHIFT) == key("ctrl+shift+w"), "canon: Ctrl+Shift+W");
    check(kmCanon('\t') == key("tab") && kmCanon('\r') == key("enter"), "canon: tab/enter stay");
    check(kmRaw(key("ctrl+c")) == 3 && kmRaw(key("shift+a")) == 'A', "raw");
    kmFormatKey(key("ctrl+shift+esc"), 1, buf, sizeof(buf));
    check(strcmp(buf, "Ctrl+Shift+Esc") == 0, "pretty format");
    kmFormatKey(key("super+enter"), 0, buf, sizeof(buf));
    check(strcmp(buf, "super+enter") == 0, "file format");

    /* 문장 */
    memset(&b, 0, sizeof(b));
    check(kmParseStmts("exec nrunner; action close", &b, err, sizeof(err)) == 0 && b.nstmt == 2 &&
          kmStmtAction(&b.s[0]) == ACT_RUN && kmStmtAction(&b.s[1]) == ACT_CLOSE, "two statements");
    check(kmParseStmts("pass; pass; pass; pass; none", &b, err, sizeof(err)) < 0, "max 4 statements");
    check(kmParseStmts("exec \"echo a; b\"; signal int; send ctrl+c; pass", &b, err, sizeof(err)) == 0 &&
          b.nstmt == 4 && strcmp(b.s[0].text, "echo a; b") == 0 && b.s[1].arg == SIGINT &&
          b.s[2].arg == key("ctrl+c") && b.s[3].op == KM_PASS, "quoted exec, signal, send, pass");
    kmFormatStmts(&b, buf, sizeof(buf));
    check(strcmp(buf, "exec \"echo a; b\"; signal INT; send ctrl+c; pass") == 0, "format statements");
    check(kmParseStmts("action window 3", &b, err, sizeof(err)) == 0 && b.s[0].arg == 3, "window n");
    check(kmParseStmts("action nope", &b, err, sizeof(err)) < 0 && strstr(err, "nope") != NULL, "unknown action");
    check(kmParseStmts("action window", &b, err, sizeof(err)) < 0, "window needs n");
    check(kmParseStmts("pass extra", &b, err, sizeof(err)) < 0, "pass takes no arg");
    check(kmParseStmts("", &b, err, sizeof(err)) < 0, "empty binding");

    /* 기본값 파일 */
    check(kmLoad(err, sizeof(err)) == 0 && !kmFromUser(), "default file parses");
    if (g_fail)
        printf("  (%s)\n", err);
    (void)kmList(&n);
    check(n > 50, "default file has the usual shortcuts");
    check(firstAct(KEY_SUPER_TAP) == ACT_MENU, "super tap = menu");
    check(firstAct('r' | KEY_MOD_SUPER) == ACT_RUN, "super+r = run (exec nrunner)");
    check(firstAct(KEY_F4 | KEY_MOD_ALT) == ACT_CLOSE, "alt+f4 = close");
    check(firstAct(KEY_CTRL('c')) == -1, "ctrl+c is not bound: goes to the program");
    k = KEY_CTRL('t') | KEY_MOD_SHIFT;
    check(firstAct(k) == -1, "ctrl+shift+t: only in a terminal window");
    g_app = "nterminal";
    check(firstAct(k) == ACT_TERMINAL, "ctrl+shift+t in a terminal window");
    check(firstAct(KEY_CTRL('q')) == -1, "ctrl+q not bound in display mode");
    g_display = 0;
    check(firstAct(KEY_CTRL('q')) == ACT_MENU, "tty: ctrl+q = menu");
    g_display = 1;

    /* 저장 -> 다시 읽기 (사용자 값이 이긴다), 기본값으로 */
    {
        static struct km_binding copy[KM_MAX];
        const struct km_binding *l = kmList(&n);
        int m;

        memcpy(copy, l, sizeof(l[0]) * (size_t)n);
        copy[0].key = key("super+x");
        check(kmSave(copy, n) == 0 && kmLoad(err, sizeof(err)) == 0 && kmFromUser(), "save + load user file");
        (void)kmList(&m);
        check(m == n && firstAct('x' | KEY_MOD_SUPER) == ACT_MENU, "round trip keeps every binding");
        check(strstr(g_user, "[super+x, super+q, alt+f1] { action menu; }") != NULL, "saved file groups keys");
        strcpy(g_user, "[super+r] { action nope; }\n");
        check(kmLoad(err, sizeof(err)) < 0 && strstr(err, "line 1") != NULL && !kmFromUser() &&
              firstAct('r' | KEY_MOD_SUPER) == ACT_RUN, "broken user file: error + defaults");
        check(kmReset() == 0 && kmLoad(err, sizeof(err)) == 0 && !kmFromUser(), "reset");
    }

    printf("keymap: %d/%d checks passed\n", g_count - g_fail, g_count);
    return g_fail ? 1 : 0;
}
