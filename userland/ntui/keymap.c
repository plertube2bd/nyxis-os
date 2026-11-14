/*
 * keymap.c - 단축키 설정: 어떤 키를 누가 받는지 (ntui 동작 / 명령 / 창의 프로그램 / 신호)
 *
 * 설정 keys (nx64_setting: 사용자 값 roi:/<uid>/app/ntui/keys 가 기본값 app:/ntui.run/keys 보다 먼저) 한 파일에
 * 모든 단축키가 있다. 둘 다 못 읽으면 아래의 내장 최소 설정을 쓴다. 형식:
 *
 *   # 줄 맨 앞(공백 뒤)의 # 은 주석
 *   [super+r, alt+f2]         { exec nrunner; }
 *   [super+l]                 { action lock; }
 *   [ctrl+shift+t @nterminal] { exec nterminal; }        @앱: 활성 창이 그 앱일 때만 (nterminal, nfiles, help ...)
 *   [tty: ctrl+q]             { action menu; }           tty: 터미널 모드에서만, display: 화면 모드에서만
 *   [ctrl+alt+c]              { signal INT; }
 *   [super+v]                 { send ctrl+shift+v; }
 *
 * 키: 수정자(super/win/meta, ctrl, alt, shift) + 키 하나를 '+' 로. 키는 글자/숫자/기호 한 글자, 또는 이름:
 *   enter tab esc space backspace delete insert home end pgup pgdn up down left right f1..f12 print menu
 *   super(혼자: Super 를 눌렀다 떼기), plus(+) comma(,) lbracket([) rbracket(]) lbrace({) rbrace(}) semicolon(;)
 *   글자는 대소문자를 가리지 않는다 (Shift 는 shift+ 로). shift+숫자/기호는 미국 배열의 그 기호로 바꾼다 (shift+1 = !).
 * 문장 ({ } 안, ';' 로 나눈다. 차례로 한다):
 *   action <이름> [n]  ntui 동작 (아래 g_acts: menu run terminal files close maximize ... window 3 keys)
 *   exec <명령>        명령 창에서 실행 (nysh -c). nrunner(실행 상자) nterminal(터미널) nfiles(파일) ntop(작업 관리자)는 ntui 안의 것
 *                      ; 나 } 가 들어가는 명령은 "..." 로 감싼다
 *   pass               그 키를 그대로 활성 창의 프로그램에게
 *   send <키>          다른 키를 활성 창의 프로그램에게 (바꿔 보내기)
 *   signal <신호>      활성 터미널 창의 포그라운드 작업에 (INT TERM KILL HUP QUIT USR1 USR2 CONT 또는 번호)
 *   none               아무것도 하지 않는다
 *
 * 바인딩이 없는 키는 활성 창의 프로그램에게 간다. 다만 화면 모드에서 Super 가 들어간 키는 데스크톱 것이라 버린다.
 * 같은 키가 여러 번 있으면 먼저 나온 것 (@앱 이 붙은 것이 붙지 않은 것보다 먼저).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <nx64.h>

#include "keymap.h"

#define KM_FILE_MAX 16384

static struct km_binding g_list[KM_MAX];
static int g_count;
static int g_fromUser;

/* ---------------- 이름 표 ---------------- */

static const struct {
    const char *file;
    const char *pretty;
    int key;
} g_names[] = {
    { "enter", "Enter", '\r' }, { "return", "Enter", '\r' }, { "tab", "Tab", '\t' },
    { "esc", "Esc", KEY_ESC }, { "escape", "Esc", KEY_ESC }, { "space", "Space", ' ' },
    { "backspace", "Backspace", 0x7F }, { "delete", "Delete", KEY_DEL }, { "del", "Delete", KEY_DEL },
    { "insert", "Insert", KEY_INS }, { "ins", "Insert", KEY_INS }, { "home", "Home", KEY_HOME },
    { "end", "End", KEY_END }, { "pgup", "PgUp", KEY_PGUP }, { "pageup", "PgUp", KEY_PGUP },
    { "pgdn", "PgDn", KEY_PGDN }, { "pagedown", "PgDn", KEY_PGDN }, { "up", "Up", KEY_UP },
    { "down", "Down", KEY_DOWN }, { "left", "Left", KEY_LEFT }, { "right", "Right", KEY_RIGHT },
    { "print", "PrintScreen", KEY_PRINT }, { "printscreen", "PrintScreen", KEY_PRINT },
    { "menu", "Menu", KEY_MENUKEY }, { "super", "Super", KEY_SUPER_TAP },
    { "plus", "Plus", '+' }, { "comma", "Comma", ',' }, { "lbracket", "[", '[' }, { "rbracket", "]", ']' },
    { "lbrace", "{", '{' }, { "rbrace", "}", '}' }, { "semicolon", ";", ';' },
};
#define NAME_COUNT ((int)(sizeof(g_names) / sizeof(g_names[0])))

static const struct {
    const char *name;
    int act;
    const char *what;
} g_acts[] = {
    { "menu", ACT_MENU, "menu (apps, theme, log out)" },
    { "run", ACT_RUN, "run a command" },
    { "terminal", ACT_TERMINAL, "new terminal" },
    { "files", ACT_FILES, "files" },
    { "close", ACT_CLOSE, "close window" },
    { "maximize", ACT_MAXIMIZE, "maximize / restore" },
    { "minimize", ACT_MINIMIZE, "minimize" },
    { "restore-or-minimize", ACT_RESTORE_MIN, "restore / minimize" },
    { "snap-left", ACT_SNAP_LEFT, "left half" },
    { "snap-right", ACT_SNAP_RIGHT, "right half" },
    { "fullscreen", ACT_FULLSCREEN, "full screen" },
    { "window-menu", ACT_WINMENU, "window menu" },
    { "move", ACT_MOVE, "move window" },
    { "resize", ACT_RESIZE, "resize window" },
    { "switch-next", ACT_SWITCH_NEXT, "switch window" },
    { "switch-prev", ACT_SWITCH_PREV, "switch back" },
    { "task-view", ACT_TASKVIEW, "window list" },
    { "next", ACT_NEXT, "next window" },
    { "prev", ACT_PREV, "previous window" },
    { "desktop", ACT_DESKTOP, "show desktop" },
    { "minimize-all", ACT_MINIMIZE_ALL, "minimize all" },
    { "restore-all", ACT_RESTORE_ALL, "restore all" },
    { "lock", ACT_LOCK, "lock screen" },
    { "task-manager", ACT_TASKMGR, "processes (ntop)" },
    { "screenshot", ACT_SCREENSHOT, "screenshot to face:/" },
    { "taskbar", ACT_TASKBAR, "taskbar" },
    { "theme", ACT_THEME, "change theme" },
    { "help", ACT_HELP, "help" },
    { "window", ACT_WINDOW, "go to window" },
    { "keys", ACT_KEYS, "shortcut settings" },
    { "reload-keys", ACT_RELOAD_KEYS, "reload shortcuts" },
};
#define ACT_NAME_COUNT ((int)(sizeof(g_acts) / sizeof(g_acts[0])))

/* exec 의 이름 중 ntui 안에 있는 것 */
static const struct {
    const char *name;
    int act;
} g_builtin[] = {
    { "nrunner", ACT_RUN }, { "nterminal", ACT_TERMINAL }, { "terminal", ACT_TERMINAL },
    { "nfiles", ACT_FILES }, { "files", ACT_FILES }, { "ntop", ACT_TASKMGR },
};

static const struct {
    const char *name;
    int sig;
} g_sigs[] = {
    { "INT", SIGINT }, { "TERM", SIGTERM }, { "KILL", SIGKILL }, { "HUP", SIGHUP }, { "QUIT", SIGQUIT },
    { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 }, { "CONT", SIGCONT }, { "WINCH", SIGWINCH },
};
#define SIG_COUNT ((int)(sizeof(g_sigs) / sizeof(g_sigs[0])))

/* 미국 배열: Shift 를 누른 기호 */
static const char g_shiftFrom[] = "1234567890-=[]\\;',./`";
static const char g_shiftTo[]   = "!@#$%^&*()_+{}|:\"<>?~";

/* ---------------- 키 ---------------- */

int kmCanon(int key)
{
    int base = KEY_BASE(key);
    int mods = key & KEY_MODS;

    if (key == KEY_SUPER_TAP || key == KEY_PRINT)
        return key;
    if (base >= 1 && base <= 26 && base != '\t' && base != '\r') {
        base = 'a' + base - 1;          /* Ctrl+글자는 제어 문자로 온다 */
        mods |= KEY_MOD_CTRL;
    } else if (base >= 0x1C && base <= 0x1F) {
        base = "\\]^_"[base - 0x1C];
        mods |= KEY_MOD_CTRL;
    } else if (base >= 'A' && base <= 'Z') {
        base += 'a' - 'A';
        mods |= KEY_MOD_SHIFT;
    }
    return base | mods;
}

int kmRaw(int canon)
{
    int base = KEY_BASE(canon);
    int mods = canon & KEY_MODS;

    if (base >= 'a' && base <= 'z' && (mods & KEY_MOD_CTRL))
        return (base & 0x1F) | (mods & ~KEY_MOD_CTRL);
    if (base >= 'a' && base <= 'z' && (mods & KEY_MOD_SHIFT))
        return (base - ('a' - 'A')) | (mods & ~KEY_MOD_SHIFT);
    return canon;
}

static int keyFromName(const char *t)
{
    int i;

    if (t[0] != '\0' && t[1] == '\0') {
        unsigned char c = (unsigned char)t[0];

        if (c > 0x20 && c < 0x7F)
            return tolower(c);
        return -1;
    }
    if (t[0] == 'f' && isdigit((unsigned char)t[1])) {
        int n = atoi(t + 1);

        if (n >= 1 && n <= 12 && (t[2] == '\0' || (isdigit((unsigned char)t[2]) && t[3] == '\0')))
            return KEY_F1 + n - 1;
        return -1;
    }
    for (i = 0; i < NAME_COUNT; i++) {
        if (strcmp(t, g_names[i].file) == 0)
            return g_names[i].key;
    }
    return -1;
}

int kmParseKey(const char *s, int *out)
{
    char tok[32];
    int mods = 0;
    int key = -1;
    int n = 0;
    const char *p = s;

    while (*p == ' ' || *p == '\t')
        p++;
    for (;;) {
        char c = *p;

        if (c == '+' || c == '\0' || c == ' ' || c == '\t') {
            tok[n] = '\0';
            if (n == 0)
                return -1;
            if (key >= 0)
                return -1;              /* 키 뒤에 또 무언가 */
            if (strcmp(tok, "super") == 0 && c == '+')
                mods |= KEY_MOD_SUPER;
            else if ((strcmp(tok, "win") == 0 || strcmp(tok, "meta") == 0) && c == '+')
                mods |= KEY_MOD_SUPER;
            else if ((strcmp(tok, "ctrl") == 0 || strcmp(tok, "control") == 0) && c == '+')
                mods |= KEY_MOD_CTRL;
            else if (strcmp(tok, "alt") == 0 && c == '+')
                mods |= KEY_MOD_ALT;
            else if (strcmp(tok, "shift") == 0 && c == '+')
                mods |= KEY_MOD_SHIFT;
            else if ((key = keyFromName(tok)) < 0)
                return -1;
            n = 0;
            if (c != '+')
                break;
            p++;
            continue;
        }
        if (n >= (int)sizeof(tok) - 1)
            return -1;
        tok[n++] = (char)tolower((unsigned char)c);
        p++;
    }
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '\0' || key < 0)
        return -1;
    if (key == KEY_SUPER_TAP && mods != 0)
        return -1;                      /* super 혼자만 */
    if ((mods & KEY_MOD_SHIFT) && key < 0x80) {
        const char *f = strchr(g_shiftFrom, key);

        if (f != NULL) {
            key = g_shiftTo[f - g_shiftFrom];
            mods &= ~KEY_MOD_SHIFT;
        }
    }
    *out = key | mods;
    return 0;
}

void kmFormatKey(int key, int pretty, char *buf, size_t cap)
{
    int base = KEY_BASE(key);
    int mods = key & KEY_MODS;
    char name[24];
    int i;

    if (key == KEY_SUPER_TAP) {
        snprintf(buf, cap, "%s", pretty ? "Super" : "super");
        return;
    }
    name[0] = '\0';
    if (base >= KEY_F1 && base <= KEY_F12) {
        snprintf(name, sizeof(name), pretty ? "F%d" : "f%d", base - KEY_F1 + 1);
    } else {
        for (i = 0; i < NAME_COUNT; i++) {
            if (g_names[i].key == base) {
                snprintf(name, sizeof(name), "%s", pretty ? g_names[i].pretty : g_names[i].file);
                break;
            }
        }
        if (name[0] == '\0' && base > 0x20 && base < 0x7F) {
            name[0] = (char)(pretty ? toupper(base) : base);
            name[1] = '\0';
        }
        if (name[0] == '\0')
            snprintf(name, sizeof(name), "?");
    }
    snprintf(buf, cap, "%s%s%s%s%s", (mods & KEY_MOD_SUPER) ? (pretty ? "Super+" : "super+") : "",
             (mods & KEY_MOD_CTRL) ? (pretty ? "Ctrl+" : "ctrl+") : "",
             (mods & KEY_MOD_ALT) ? (pretty ? "Alt+" : "alt+") : "",
             (mods & KEY_MOD_SHIFT) ? (pretty ? "Shift+" : "shift+") : "", name);
}

/* ---------------- 문장 ---------------- */

static void setErr(char *err, size_t cap, const char *fmt, const char *arg)
{
    if (err != NULL && cap > 0)
        snprintf(err, cap, fmt, arg);
}

static int sigFromName(const char *s)
{
    int i;
    char *end;
    long v;

    if (strncmp(s, "SIG", 3) == 0 || strncmp(s, "sig", 3) == 0)
        s += 3;
    for (i = 0; i < SIG_COUNT; i++) {
        const char *a = g_sigs[i].name;
        const char *b = s;

        while (*a && toupper((unsigned char)*b) == *a) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0')
            return g_sigs[i].sig;
    }
    v = strtol(s, &end, 10);
    return (end != s && *end == '\0' && v > 0 && v < 32) ? (int)v : -1;
}

static int parseOne(const char *word, const char *arg, struct km_stmt *st, char *err, size_t cap)
{
    memset(st, 0, sizeof(*st));
    if (strcmp(word, "pass") == 0 || strcmp(word, "none") == 0) {
        if (arg[0] != '\0') {
            setErr(err, cap, "'%s' takes no argument", word);
            return -1;
        }
        st->op = word[0] == 'p' ? KM_PASS : KM_NOTHING;
        return 0;
    }
    if (arg[0] == '\0') {
        setErr(err, cap, "'%s' needs an argument", word);
        return -1;
    }
    if (strcmp(word, "exec") == 0) {
        if (strlen(arg) >= KM_TEXT) {
            setErr(err, cap, "%s", "command too long");
            return -1;
        }
        st->op = KM_EXEC;
        strcpy(st->text, arg);
        return 0;
    }
    if (strcmp(word, "send") == 0) {
        st->op = KM_SEND;
        if (kmParseKey(arg, &st->arg) < 0) {
            setErr(err, cap, "unknown key '%s'", arg);
            return -1;
        }
        return 0;
    }
    if (strcmp(word, "signal") == 0) {
        st->op = KM_SIGNAL;
        if ((st->arg = sigFromName(arg)) < 0) {
            setErr(err, cap, "unknown signal '%s'", arg);
            return -1;
        }
        return 0;
    }
    if (strcmp(word, "action") == 0) {
        char name[32];
        const char *sp = strchr(arg, ' ');
        size_t n = sp ? (size_t)(sp - arg) : strlen(arg);
        int i;

        if (n >= sizeof(name)) {
            setErr(err, cap, "unknown action '%s'", arg);
            return -1;
        }
        memcpy(name, arg, n);
        name[n] = '\0';
        st->op = KM_ACTION;
        for (i = 0; i < ACT_NAME_COUNT; i++) {
            if (strcmp(name, g_acts[i].name) == 0)
                break;
        }
        if (i == ACT_NAME_COUNT) {
            setErr(err, cap, "unknown action '%s'", name);
            return -1;
        }
        st->act = g_acts[i].act;
        if (st->act == ACT_WINDOW) {
            char *end;
            long v = sp ? strtol(sp + 1, &end, 10) : 0;

            if (sp == NULL || *end != '\0' || v < 1 || v > KM_WINDOW_MAX) {
                setErr(err, cap, "%s", "action window needs a number (1..12)");
                return -1;
            }
            st->arg = (int)v;
        } else if (sp != NULL) {
            setErr(err, cap, "action '%s' takes no number", name);
            return -1;
        }
        return 0;
    }
    setErr(err, cap, "unknown command '%s' (action/exec/pass/send/signal/none)", word);
    return -1;
}

/* s[0..len) 의 문장들을 b 에 */
static int parseStmtsN(const char *s, size_t len, struct km_binding *b, char *err, size_t cap)
{
    size_t i = 0;

    b->nstmt = 0;
    while (i < len) {
        char word[16];
        char arg[KM_TEXT + 8];
        size_t w = 0;
        size_t a = 0;

        while (i < len && isspace((unsigned char)s[i]))
            i++;
        if (i < len && s[i] == '#') {           /* 블록 안의 주석 줄 */
            while (i < len && s[i] != '\n')
                i++;
            continue;
        }
        if (i >= len)
            break;
        if (s[i] == ';') {
            i++;
            continue;
        }
        while (i < len && (isalpha((unsigned char)s[i]) || s[i] == '-') && w < sizeof(word) - 1)
            word[w++] = s[i++];
        word[w] = '\0';
        if (w == 0) {
            if (err != NULL && cap > 0)
                snprintf(err, cap, "unexpected '%c'", s[i]);
            return -1;
        }
        while (i < len && (s[i] == ' ' || s[i] == '\t'))
            i++;
        if (i < len && s[i] == '"') {
            i++;
            while (i < len && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < len)
                    i++;
                if (a < sizeof(arg) - 1)
                    arg[a++] = s[i];
                i++;
            }
            if (i >= len) {
                setErr(err, cap, "%s", "missing closing \"");
                return -1;
            }
            i++;
        } else {
            while (i < len && s[i] != ';' && s[i] != '\n') {
                if (a < sizeof(arg) - 1)
                    arg[a++] = s[i];
                i++;
            }
            while (a > 0 && isspace((unsigned char)arg[a - 1]))
                a--;
        }
        arg[a] = '\0';
        if (a >= KM_TEXT) {
            setErr(err, cap, "%s", "argument too long");
            return -1;
        }
        if (b->nstmt >= KM_STMTS) {
            setErr(err, cap, "%s", "too many commands in one binding (max 4)");
            return -1;
        }
        if (parseOne(word, arg, &b->s[b->nstmt], err, cap) < 0)
            return -1;
        b->nstmt++;
        while (i < len && (s[i] == ' ' || s[i] == '\t'))
            i++;
        if (i < len && s[i] != ';' && s[i] != '\n') {
            setErr(err, cap, "%s", "expected ';'");
            return -1;
        }
    }
    if (b->nstmt == 0) {
        setErr(err, cap, "%s", "empty binding (use 'none' to ignore a key)");
        return -1;
    }
    return 0;
}

int kmParseStmts(const char *s, struct km_binding *b, char *err, size_t cap)
{
    return parseStmtsN(s, strlen(s), b, err, cap);
}

static void quoteArg(const char *t, char *out, size_t cap)
{
    size_t n = 0;
    const char *p;
    int need = t[0] == '\0' || strpbrk(t, ";}\"\n") != NULL || t[0] == ' ';

    if (!need) {
        snprintf(out, cap, "%s", t);
        return;
    }
    if (cap < 3) {
        out[0] = '\0';
        return;
    }
    out[n++] = '"';
    for (p = t; *p && n + 3 < cap; p++) {
        if (*p == '"' || *p == '\\')
            out[n++] = '\\';
        out[n++] = *p;
    }
    out[n++] = '"';
    out[n] = '\0';
}

static void formatStmt(const struct km_stmt *st, char *buf, size_t cap)
{
    char tmp[KM_TEXT * 2];
    int i;

    switch (st->op) {
    case KM_ACTION:
        for (i = 0; i < ACT_NAME_COUNT; i++) {
            if (g_acts[i].act == st->act)
                break;
        }
        if (st->act == ACT_WINDOW)
            snprintf(buf, cap, "action window %d", st->arg);
        else
            snprintf(buf, cap, "action %s", i < ACT_NAME_COUNT ? g_acts[i].name : "?");
        return;
    case KM_EXEC:
        quoteArg(st->text, tmp, sizeof(tmp));
        snprintf(buf, cap, "exec %s", tmp);
        return;
    case KM_PASS:
        snprintf(buf, cap, "pass");
        return;
    case KM_SEND:
        kmFormatKey(st->arg, 0, tmp, sizeof(tmp));
        snprintf(buf, cap, "send %s", tmp);
        return;
    case KM_SIGNAL:
        for (i = 0; i < SIG_COUNT; i++) {
            if (g_sigs[i].sig == st->arg) {
                snprintf(buf, cap, "signal %s", g_sigs[i].name);
                return;
            }
        }
        snprintf(buf, cap, "signal %d", st->arg);
        return;
    default:
        snprintf(buf, cap, "none");
        return;
    }
}

void kmFormatStmts(const struct km_binding *b, char *buf, size_t cap)
{
    char one[KM_TEXT * 2 + 16];
    int i;

    buf[0] = '\0';
    for (i = 0; i < b->nstmt; i++) {
        formatStmt(&b->s[i], one, sizeof(one));
        if (strlen(buf) + strlen(one) + 3 >= cap)
            break;
        if (i > 0)
            strcat(buf, "; ");
        strcat(buf, one);
    }
}

int kmStmtAction(const struct km_stmt *st)
{
    size_t i;

    if (st->op == KM_ACTION)
        return st->act;
    if (st->op == KM_EXEC) {
        for (i = 0; i < sizeof(g_builtin) / sizeof(g_builtin[0]); i++) {
            if (strcmp(st->text, g_builtin[i].name) == 0)
                return g_builtin[i].act;
        }
    }
    return ACT_NONE;
}

static const char *actWhat(int act)
{
    int i;

    for (i = 0; i < ACT_NAME_COUNT; i++) {
        if (g_acts[i].act == act)
            return g_acts[i].what;
    }
    return "?";
}

void kmDescribe(const struct km_binding *b, char *buf, size_t cap)
{
    char one[KM_TEXT + 32];
    char k[32];
    int i;

    buf[0] = '\0';
    for (i = 0; i < b->nstmt; i++) {
        const struct km_stmt *st = &b->s[i];
        int act = kmStmtAction(st);

        if (act == ACT_WINDOW)
            snprintf(one, sizeof(one), "go to window %d", st->arg);
        else if (act != ACT_NONE)
            snprintf(one, sizeof(one), "%s", actWhat(act));
        else if (st->op == KM_EXEC)
            snprintf(one, sizeof(one), "run %s", st->text);
        else if (st->op == KM_PASS)
            snprintf(one, sizeof(one), "to the program");
        else if (st->op == KM_SEND) {
            kmFormatKey(st->arg, 1, k, sizeof(k));
            snprintf(one, sizeof(one), "send %s to the program", k);
        } else if (st->op == KM_SIGNAL) {
            formatStmt(st, one, sizeof(one));
            strcat(one, " to the job");
        } else
            snprintf(one, sizeof(one), "nothing");
        if (strlen(buf) + strlen(one) + 3 >= cap)
            break;
        if (i > 0)
            strcat(buf, ", ");
        strcat(buf, one);
    }
    if (b->app[0] != '\0' && strlen(buf) + strlen(b->app) + 8 < cap) {
        strcat(buf, " (in ");
        strcat(buf, b->app);
        strcat(buf, ")");
    }
}

/* ---------------- 파일 ---------------- */

static int lineOf(const char *t, size_t i)
{
    int n = 1;
    size_t k;

    for (k = 0; k < i; k++)
        n += t[k] == '\n';
    return n;
}

static int parseText(const char *t, struct km_binding *out, int *count, char *err, size_t cap)
{
    size_t i = 0;
    size_t len = strlen(t);
    char msg[96];

    *count = 0;
    for (;;) {
        size_t hs;
        size_t he;
        size_t bs;
        size_t be;
        int mode = KM_MODE_ALL;
        char app[16];
        char head[256];
        char *h;
        char *at;
        char *key;
        struct km_binding proto;
        int quoted = 0;

        while (i < len && isspace((unsigned char)t[i]))
            i++;
        if (i < len && t[i] == '#') {
            while (i < len && t[i] != '\n')
                i++;
            continue;
        }
        if (i >= len)
            break;
        if (t[i] != '[') {
            snprintf(err, cap, "line %d: expected '[key]'", lineOf(t, i));
            return -1;
        }
        hs = ++i;
        while (i < len && t[i] != ']' && t[i] != '\n')
            i++;
        if (i >= len || t[i] != ']') {
            snprintf(err, cap, "line %d: missing ']'", lineOf(t, hs));
            return -1;
        }
        he = i++;
        while (i < len && isspace((unsigned char)t[i]))
            i++;
        if (i >= len || t[i] != '{') {
            snprintf(err, cap, "line %d: expected '{' after the keys", lineOf(t, i < len ? i : len));
            return -1;
        }
        bs = ++i;
        while (i < len && (quoted || t[i] != '}')) {
            if (t[i] == '"')
                quoted = !quoted;
            else if (t[i] == '\\' && quoted && i + 1 < len)
                i++;
            i++;
        }
        if (i >= len) {
            snprintf(err, cap, "line %d: missing '}'", lineOf(t, bs));
            return -1;
        }
        be = i++;

        /* 머리: [tty: a, b @app] */
        if (he - hs >= sizeof(head)) {
            snprintf(err, cap, "line %d: key list too long", lineOf(t, hs));
            return -1;
        }
        memcpy(head, t + hs, he - hs);
        head[he - hs] = '\0';
        h = head;
        while (*h == ' ')
            h++;
        if (strncmp(h, "tty:", 4) == 0) {
            mode = KM_MODE_TTY;
            h += 4;
        } else if (strncmp(h, "display:", 8) == 0) {
            mode = KM_MODE_DISPLAY;
            h += 8;
        } else if (strncmp(h, "all:", 4) == 0) {
            h += 4;
        }
        app[0] = '\0';
        at = strchr(h, '@');                    /* @앱 은 키 목록 끝에 */
        if (at != NULL) {
            char *a = at + 1;
            size_t n = strcspn(a, " ");

            if (n == 0 || n >= sizeof(app) || a[n + strspn(a + n, " ")] != '\0') {
                snprintf(err, cap, "line %d: bad window filter '@%s'", lineOf(t, hs), a);
                return -1;
            }
            memcpy(app, a, n);
            app[n] = '\0';
            *at = '\0';
        }

        memset(&proto, 0, sizeof(proto));
        if (parseStmtsN(t + bs, be - bs, &proto, msg, sizeof(msg)) < 0) {
            snprintf(err, cap, "line %d: %s", lineOf(t, bs), msg);
            return -1;
        }
        proto.mode = mode;
        strcpy(proto.app, app);

        for (key = strtok(h, ","); key != NULL; key = strtok(NULL, ",")) {
            int k;

            if (kmParseKey(key, &k) < 0) {
                while (*key == ' ')
                    key++;
                snprintf(err, cap, "line %d: unknown key '%.40s'", lineOf(t, hs), key);
                return -1;
            }
            if (*count >= KM_MAX) {
                snprintf(err, cap, "too many bindings (max %d)", KM_MAX);
                return -1;
            }
            out[*count] = proto;
            out[*count].key = k;
            (*count)++;
        }
    }
    return 0;
}

/* 파일 내용을 다시 만든다: 같은 모드/창/문장인 이웃 바인딩은 한 줄로 묶는다 */
static void formatText(const struct km_binding *list, int n, char *out, size_t cap)
{
    size_t len = 0;
    int i = 0;

    len += (size_t)snprintf(out, cap, "# ntui shortcuts (saved by the shortcut settings window, Super+I).\n"
                                      "# Format: [keys] { commands; }  - see app:/ntui.run/keys for the full description.\n\n");
    while (i < n && len < cap) {
        char keys[256];
        char stmts[KM_STMTS * (KM_TEXT * 2 + 16)];
        char k[32];
        int j = i;

        keys[0] = '\0';
        kmFormatStmts(&list[i], stmts, sizeof(stmts));
        while (j < n && list[j].mode == list[i].mode && strcmp(list[j].app, list[i].app) == 0) {
            char other[sizeof(stmts)];

            kmFormatStmts(&list[j], other, sizeof(other));
            if (strcmp(other, stmts) != 0)
                break;
            kmFormatKey(list[j].key, 0, k, sizeof(k));
            if (strlen(keys) + strlen(k) + 3 >= sizeof(keys))
                break;
            if (keys[0])
                strcat(keys, ", ");
            strcat(keys, k);
            j++;
        }
        if (j == i)
            j = i + 1;
        len += (size_t)snprintf(out + len, cap - len, "[%s%s%s%s] { %s; }\n",
                                list[i].mode == KM_MODE_TTY ? "tty: " : list[i].mode == KM_MODE_DISPLAY ? "display: " : "",
                                keys, list[i].app[0] ? " @" : "", list[i].app, stmts);
        i = j;
    }
}

/* 둘 다 못 읽을 때 (파일이 없거나 깨짐): 기본 동작만 */
static const char g_builtinText[] =
    "[super, super+q, alt+f1] { action menu; }\n"
    "[super+r, alt+f2] { exec nrunner; }\n"
    "[super+enter, ctrl+alt+t] { exec nterminal; }\n"
    "[super+e] { exec nfiles; }\n"
    "[alt+f4] { action close; }\n"
    "[alt+tab] { action switch-next; }\n"
    "[alt+shift+tab] { action switch-prev; }\n"
    "[super+l] { action lock; }\n"
    "[super+i] { action keys; }\n"
    "[f1] { action help; }\n"
    "[tty: ctrl+q] { action menu; }\n"
    "[tty: ctrl+r] { exec nrunner; }\n"
    "[tty: ctrl+t] { exec nterminal; }\n"
    "[tty: ctrl+w] { action close; }\n"
    "[tty: ctrl+n] { action next; }\n";

int kmLoad(char *err, size_t cap)
{
    static char text[KM_FILE_MAX];
    static struct km_binding tmp[KM_MAX];
    char why[128];
    int n = 0;
    int rc = 0;

    why[0] = '\0';
    if (nyx_setting_get_from(NYX_SETTING_USER, "ntui", "keys", text, sizeof(text)) >= 0) {
        if (parseText(text, tmp, &n, why, sizeof(why)) == 0) {
            memcpy(g_list, tmp, sizeof(tmp[0]) * (size_t)n);
            g_count = n;
            g_fromUser = 1;
            return 0;
        }
        snprintf(err, cap, "your keys: %s (using defaults)", why);
        rc = -1;
    }
    g_fromUser = 0;
    if (nyx_setting_get_from(NYX_SETTING_DEFAULT, "ntui", "keys", text, sizeof(text)) >= 0) {
        if (parseText(text, tmp, &n, why, sizeof(why)) == 0) {
            memcpy(g_list, tmp, sizeof(tmp[0]) * (size_t)n);
            g_count = n;
            return rc;
        }
        snprintf(err, cap, "app:/ntui.run/keys: %s (using built-in keys)", why);
    } else if (rc == 0) {
        snprintf(err, cap, "no shortcut file (using built-in keys)");
    }
    (void)parseText(g_builtinText, tmp, &n, why, sizeof(why));
    memcpy(g_list, tmp, sizeof(tmp[0]) * (size_t)n);
    g_count = n;
    return -1;
}

int kmFromUser(void)
{
    return g_fromUser;
}

const struct km_binding *kmList(int *n)
{
    *n = g_count;
    return g_list;
}

int kmModeApplies(int mode, int display)
{
    return mode == KM_MODE_ALL || (mode == KM_MODE_DISPLAY) == (display != 0);
}

const struct km_binding *kmLookup(int key, const char *app, int display)
{
    int c = kmCanon(key);
    int pass;
    int i;

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < g_count; i++) {
            const struct km_binding *b = &g_list[i];

            if (b->key != c || !kmModeApplies(b->mode, display))
                continue;
            if (pass == 0 ? (b->app[0] != '\0' && strcmp(b->app, app) == 0) : b->app[0] == '\0')
                return b;
        }
    }
    return NULL;
}

int kmSave(const struct km_binding *list, int n)
{
    static char text[KM_FILE_MAX];
    size_t len;

    formatText(list, n, text, sizeof(text));
    len = strlen(text);
    if (len > 0 && text[len - 1] == '\n')
        text[len - 1] = '\0';           /* nx64_setting_set 이 개행을 붙인다 */
    return nx64_setting_set("ntui", "keys", text);
}

int kmReset(void)
{
    return nx64_setting_reset("ntui", "keys");
}
