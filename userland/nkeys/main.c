/*
 * nkeys - 단축키 설정 창 (NWP 창 프로그램: ntui 의 Super+I, 메뉴의 Shortcuts)
 *   nkeys      설정 창                nkeys -h   단축키 보기 (도움말, F1)
 * 저장하면 컴포지터에게 다시 읽어 달라고 한다 (NWP_REQ_RELOAD). 키를 녹음/편집하는 동안은 단축키를 막는다
 * (NWP_REQ_INHIBIT): Super+R 같은 데스크톱 키도 이 창으로 온다.
 *
 * 지금 쓰는 단축키 목록(keymap.c)을 사본으로 가져와 고치고, s 로 사용자 값(roi:/<uid>/app/ntui/keys)에 저장한다.
 * 저장하면 바로 다시 읽어 쓴다. 기본값(app:/ntui.run/keys)은 고치지 않는다: r 은 사용자 값을 지워 기본값으로 돌린다.
 *
 *   ↑ ↓ PgUp PgDn Home End  고르기
 *   a / Insert   새 단축키: 키를 누르고(녹음) 명령을 쓴다
 *   Enter / e    명령 고치기 ("exec nrunner; action close" 처럼 ';' 로)
 *   k            키 바꾸기 (녹음)
 *   m            어디서: all -> display -> tty
 *   w            어느 창에서: 모두 -> nterminal -> nfiles
 *   d / Delete   지우기      s 저장      r 기본값으로 (y 로 확인)      u 저장하지 않은 것 버리기
 *   q / Esc      닫기 (저장하지 않은 것이 있으면 묻는다)
 * 녹음/편집 중에는 단축키보다 이 창이 먼저 모든 키를 받는다 (keysWantsAllKeys): Super+R 처럼 데스크톱 키도 녹음된다.
 * 녹음 중 Esc 는 취소다 (Esc 자체는 파일에서 묶는다).
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <nx64.h>

#define NWP_SHORT_NAMES
#include <nwp.h>

#include "keymap.h"

/* 창 (NWP) */
static struct nwp_window *g_w;
struct window {
    void *data;
    int wantClose;
};
#define screenText(x, y, s, r)          nwp_text(g_w, (x), (y), (s), (r))
#define screenFill(x, y, cw, chh, c, r) nwp_fill(g_w, (x), (y), (cw), (chh), (c), (r))
#define leDraw(le, x, y, w, sec, cur)   nwp_le_draw(g_w, (le), (x), (y), (w), (sec), (cur))
#define leInit  nwp_le_init
#define leSet   nwp_le_set
#define leKey   nwp_le_key
#define lineedit nwp_lineedit

enum { K_LIST, K_CAPTURE_NEW, K_CAPTURE_KEY, K_EDIT, K_CONFIRM_RESET, K_CONFIRM_CLOSE };

struct kset {
    struct km_binding list[KM_MAX];
    int count;
    int sel;
    int top;
    int rows;
    int mode;
    int dirty;
    int editNew;                        /* 편집 중인 것이 새 바인딩이다 (pending) */
    struct km_binding pending;
    char edit[KM_STMTS * (KM_TEXT * 2 + 16)];
    struct lineedit le;
    char msg[120];
};

static struct kset g_k;

static void reloadCopy(void)
{
    int n;
    const struct km_binding *l = kmList(&n);

    memcpy(g_k.list, l, sizeof(l[0]) * (size_t)n);
    g_k.count = n;
    g_k.dirty = 0;
    if (g_k.sel >= n)
        g_k.sel = n > 0 ? n - 1 : 0;
}

/* 키를 녹음/편집하는 동안은 컴포지터의 단축키를 막고 모든 키를 받는다 (NWP_REQ_INHIBIT) */
static int wantsAllKeys(void)
{
    return g_k.mode == K_CAPTURE_NEW || g_k.mode == K_CAPTURE_KEY || g_k.mode == K_EDIT;
}

/* 같은 키/모드/창의 다른 바인딩을 지운다 (먼저 나온 것이 이기므로 새것이 가려지지 않게). 지운 수 */
static int removeDuplicates(int keep)
{
    const struct km_binding *b = &g_k.list[keep];
    int removed = 0;
    int i;

    for (i = g_k.count - 1; i >= 0; i--) {
        const struct km_binding *o = &g_k.list[i];

        if (i == keep || o->key != b->key || strcmp(o->app, b->app) != 0)
            continue;
        if (o->mode != KM_MODE_ALL && b->mode != KM_MODE_ALL && o->mode != b->mode)
            continue;
        memmove(&g_k.list[i], &g_k.list[i + 1], sizeof(g_k.list[0]) * (size_t)(g_k.count - i - 1));
        g_k.count--;
        if (i < keep)
            keep--;
        removed++;
    }
    g_k.sel = keep;
    return removed;
}

static void startEdit(int isNew)
{
    const struct km_binding *b = isNew ? &g_k.pending : &g_k.list[g_k.sel];

    g_k.editNew = isNew;
    leInit(&g_k.le, g_k.edit, (int)sizeof(g_k.edit));
    if (b->nstmt > 0) {
        char tmp[sizeof(g_k.edit)];

        kmFormatStmts(b, tmp, sizeof(tmp));
        leSet(&g_k.le, tmp);
    }
    g_k.mode = K_EDIT;
    g_k.msg[0] = '\0';
}

static void save(void)
{
    char err[128];

    if (kmSave(g_k.list, g_k.count) < 0) {
        snprintf(g_k.msg, sizeof(g_k.msg), "save failed: %s", strerror(errno));
        return;
    }
    nwp_reload_config(g_w);             /* 컴포지터도 다시 읽는다 */
    if (kmLoad(err, sizeof(err)) < 0) {
        snprintf(g_k.msg, sizeof(g_k.msg), "%s", err);
        return;
    }
    reloadCopy();
    snprintf(g_k.msg, sizeof(g_k.msg), "saved and in use (%d keys)", g_k.count);
}

/* ---------------- 그리기 ---------------- */

static const char *modeName(const struct km_binding *b)
{
    return b->mode == KM_MODE_TTY ? "tty" : b->mode == KM_MODE_DISPLAY ? "display" : "all";
}

static void keysDraw(struct window *w, int x, int y, int cols, int rows, int active)
{
    char line[256];
    char key[40];
    char where[32];
    char what[KM_STMTS * (KM_TEXT * 2 + 16)];
    int listRows = rows - 4;
    int i;
    int r;

    (void)w;
    (void)active;
    if (cols < 20 || rows < 6)
        return;
    snprintf(line, sizeof(line), " %d shortcuts from %s%s", g_k.count,
             kmFromUser() ? "your file (roi:/<uid>/app/ntui/keys)" : "the defaults (app:/ntui.run/keys)",
             g_k.dirty ? "  [not saved]" : "");
    screenFill(x, y, cols, 1, ' ', ROLE_TITLE_INACTIVE);
    screenText(x, y, line, ROLE_TITLE_INACTIVE);
    snprintf(line, sizeof(line), " %-22s %-17s %s", "KEY", "WHERE", "COMMANDS");
    screenText(x, y + 1, line, ROLE_BORDER);

    g_k.rows = listRows;
    if (g_k.sel < g_k.top)
        g_k.top = g_k.sel;
    if (g_k.sel >= g_k.top + listRows)
        g_k.top = g_k.sel - listRows + 1;
    for (r = 0, i = g_k.top; r < listRows && i < g_k.count; r++, i++) {
        const struct km_binding *b = &g_k.list[i];
        int role = i == g_k.sel ? ROLE_SELECTION : ROLE_WINDOW;

        kmFormatKey(b->key, 1, key, sizeof(key));
        snprintf(where, sizeof(where), "%s%s%s", modeName(b), b->app[0] ? " @" : "", b->app);
        kmFormatStmts(b, what, sizeof(what));
        snprintf(line, sizeof(line), " %-22s %-17s %s", key, where, what);
        if ((int)strlen(line) > cols)
            line[cols] = '\0';
        if (i == g_k.sel)
            screenFill(x, y + 2 + r, cols, 1, ' ', role);
        screenText(x, y + 2 + r, line, role);
    }

    /* 아래 두 줄: 상태 / 안내 */
    screenFill(x, y + rows - 2, cols, 2, ' ', ROLE_WINDOW);
    switch (g_k.mode) {
    case K_CAPTURE_NEW:
    case K_CAPTURE_KEY:
        screenText(x + 1, y + rows - 2, "Press the key combination now  (Esc cancels)", ROLE_ERROR);
        break;
    case K_EDIT:
        if (g_k.editNew) {
            kmFormatKey(g_k.pending.key, 1, key, sizeof(key));
            snprintf(line, sizeof(line), "%s:", key);
        } else {
            snprintf(line, sizeof(line), "commands:");
        }
        screenText(x + 1, y + rows - 2, line, ROLE_BORDER);
        leDraw(&g_k.le, x + 2 + (int)strlen(line), y + rows - 2, cols - 3 - (int)strlen(line), 0, 1);
        break;
    case K_CONFIRM_RESET:
        screenText(x + 1, y + rows - 2, "Delete your shortcuts and use the defaults? y = yes", ROLE_ERROR);
        break;
    case K_CONFIRM_CLOSE:
        screenText(x + 1, y + rows - 2, "Close without saving? y = yes, s = save and close", ROLE_ERROR);
        break;
    default:
        if (g_k.msg[0])
            screenText(x + 1, y + rows - 2, g_k.msg, ROLE_SELECTION);
        break;
    }
    if (g_k.mode == K_EDIT)
        screenText(x + 1, y + rows - 1,
                   "action <name> | exec <cmd> | pass | send <key> | signal <SIG> | none   Enter ok  Esc cancel",
                   ROLE_BORDER);
    else
        screenText(x + 1, y + rows - 1,
                   "a add  Enter edit  k key  m mode  w window  d delete  s save  r defaults  u undo  q close",
                   ROLE_BORDER);
}

/* ---------------- 키 ---------------- */

static void capture(int key)
{
    int canon;
    int isNew = g_k.mode == K_CAPTURE_NEW;

    if (key == KEY_ALT_UP)
        return;                         /* Alt 를 뗀 것: 아직 키를 기다린다 */
    if (key == KEY_ESC) {
        g_k.mode = K_LIST;
        g_k.msg[0] = '\0';
        return;
    }
    canon = kmCanon(key);
    if (isNew) {
        memset(&g_k.pending, 0, sizeof(g_k.pending));
        g_k.pending.key = canon;
        startEdit(1);
        return;
    }
    g_k.list[g_k.sel].key = canon;
    g_k.dirty = 1;
    g_k.mode = K_LIST;
    if (removeDuplicates(g_k.sel) > 0)
        snprintf(g_k.msg, sizeof(g_k.msg), "key changed (replaced an older binding of the same key)");
    else
        snprintf(g_k.msg, sizeof(g_k.msg), "key changed");
}

static void finishEdit(void)
{
    struct km_binding b;
    char err[96];

    g_k.edit[g_k.le.len] = '\0';
    b = g_k.editNew ? g_k.pending : g_k.list[g_k.sel];
    if (kmParseStmts(g_k.edit, &b, err, sizeof(err)) < 0) {
        snprintf(g_k.msg, sizeof(g_k.msg), "%s", err);
        return;                         /* 편집을 계속한다 */
    }
    if (g_k.editNew) {
        if (g_k.count >= KM_MAX) {
            snprintf(g_k.msg, sizeof(g_k.msg), "too many shortcuts");
            g_k.mode = K_LIST;
            return;
        }
        g_k.list[g_k.count++] = b;
        g_k.sel = g_k.count - 1;
        if (removeDuplicates(g_k.sel) > 0)
            snprintf(g_k.msg, sizeof(g_k.msg), "added (replaced an older binding of the same key)  s to save");
        else
            snprintf(g_k.msg, sizeof(g_k.msg), "added  s to save");
    } else {
        g_k.list[g_k.sel] = b;
        snprintf(g_k.msg, sizeof(g_k.msg), "changed  s to save");
    }
    g_k.dirty = 1;
    g_k.mode = K_LIST;
}

static int keysKey(struct window *w, int key)
{
    int base = KEY_BASE(key);
    int page = g_k.rows > 1 ? g_k.rows - 1 : 1;
    static const char *const apps[] = { "", "nterminal", "nfiles" };

    if (g_k.mode == K_CAPTURE_NEW || g_k.mode == K_CAPTURE_KEY) {
        capture(key);
        return 1;
    }
    if (g_k.mode == K_EDIT) {
        if (base == '\r')
            finishEdit();
        else if (key == KEY_ESC) {
            g_k.mode = K_LIST;
            g_k.msg[0] = '\0';
        } else
            (void)leKey(&g_k.le, key);
        return 1;
    }
    if (g_k.mode == K_CONFIRM_RESET) {
        g_k.mode = K_LIST;
        if (base == 'y' || base == 'Y') {
            char err[128];

            nwp_reload_config(g_w);
            if (kmReset() < 0) {
                snprintf(g_k.msg, sizeof(g_k.msg), "reset failed: %s", strerror(errno));
            } else {
                if (kmLoad(err, sizeof(err)) < 0)
                    snprintf(g_k.msg, sizeof(g_k.msg), "%s", err);
                else
                    snprintf(g_k.msg, sizeof(g_k.msg), "back to the defaults");
                reloadCopy();
            }
        }
        return 1;
    }
    if (g_k.mode == K_CONFIRM_CLOSE) {
        g_k.mode = K_LIST;
        if (base == 's')
            save();
        if (base == 'y' || base == 'Y' || (base == 's' && !g_k.dirty))
            w->wantClose = 1;
        return 1;
    }

    g_k.msg[0] = '\0';
    switch (base) {
    case KEY_UP:   g_k.sel--; break;
    case KEY_DOWN: g_k.sel++; break;
    case KEY_PGUP: g_k.sel -= page; break;
    case KEY_PGDN: g_k.sel += page; break;
    case KEY_HOME: g_k.sel = 0; break;
    case KEY_END:  g_k.sel = g_k.count - 1; break;
    case 'a':
    case KEY_INS:
        g_k.mode = K_CAPTURE_NEW;
        break;
    case '\r':
    case 'e':
        if (g_k.count > 0)
            startEdit(0);
        break;
    case 'k':
        if (g_k.count > 0)
            g_k.mode = K_CAPTURE_KEY;
        break;
    case 'm':
        if (g_k.count > 0) {
            struct km_binding *b = &g_k.list[g_k.sel];

            b->mode = (b->mode + 1) % 3;
            g_k.dirty = 1;
        }
        break;
    case 'w':
        if (g_k.count > 0) {
            struct km_binding *b = &g_k.list[g_k.sel];
            int i;
            int next = 0;

            for (i = 0; i < 3; i++) {
                if (strcmp(b->app, apps[i]) == 0)
                    next = (i + 1) % 3;
            }
            strcpy(b->app, apps[next]);
            g_k.dirty = 1;
        }
        break;
    case 'd':
    case KEY_DEL:
        if (g_k.count > 0) {
            memmove(&g_k.list[g_k.sel], &g_k.list[g_k.sel + 1],
                    sizeof(g_k.list[0]) * (size_t)(g_k.count - g_k.sel - 1));
            g_k.count--;
            g_k.dirty = 1;
            snprintf(g_k.msg, sizeof(g_k.msg), "deleted  s to save");
        }
        break;
    case 's':
        save();
        break;
    case 'r':
        g_k.mode = K_CONFIRM_RESET;
        break;
    case 'u':
        reloadCopy();
        snprintf(g_k.msg, sizeof(g_k.msg), "changes discarded");
        break;
    case 'q':
    case KEY_ESC:
        if (g_k.dirty)
            g_k.mode = K_CONFIRM_CLOSE;
        else
            w->wantClose = 1;
        break;
    default:
        break;
    }
    if (g_k.sel >= g_k.count)
        g_k.sel = g_k.count - 1;
    if (g_k.sel < 0)
        g_k.sel = 0;
    return 1;
}

/* 동작 action 을 하는 단축키 이름 ("Super+I"), 없으면 "-" */
static const char *keyLabel(int action)
{
    static char buf[32];
    const struct km_binding *list;
    int n;
    int i;

    list = kmList(&n);
    for (i = 0; i < n; i++) {
        const struct km_binding *b = &list[i];

        if (b->nstmt == 1 && b->app[0] == '\0' && kmModeApplies(b->mode, nwp_display_mode(g_w)) &&
            kmStmtAction(&b->s[0]) == action) {
            kmFormatKey(b->key, 1, buf, sizeof(buf));
            return buf;
        }
    }
    return "-";
}

static char g_user[64];
static char g_host[64];

/* ---------------- 도움말 창 ---------------- */

/*
 * 지금 모드의 단축키를 설명과 함께 보인다 (같은 일을 하는 키는 한 줄로 묶는다). ↑ ↓ PgUp PgDn 으로 넘긴다.
 * 고치려면 단축키 설정 창 (keyLabel(ACT_KEYS)).
 */
static int g_helpTop;

/* 도움말 줄 k 를 만든다. 줄이 없으면 0 */
static int helpLine(int k, char *keys, size_t kc, char *what, size_t wc)
{
    const struct km_binding *list;
    int n;
    int i;
    int j;
    int idx = 0;
    char a[KM_STMTS * (KM_TEXT * 2 + 16)];
    char b2[sizeof(a)];
    char one[32];

    list = kmList(&n);
    for (i = 0; i < n; i++) {
        int seen = 0;

        if (!kmModeApplies(list[i].mode, nwp_display_mode(g_w)))
            continue;
        kmFormatStmts(&list[i], a, sizeof(a));
        for (j = 0; j < i && !seen; j++) {          /* 앞에서 이미 같은 설명으로 나왔다 */
            if (!kmModeApplies(list[j].mode, nwp_display_mode(g_w)) || strcmp(list[j].app, list[i].app) != 0)
                continue;
            kmFormatStmts(&list[j], b2, sizeof(b2));
            seen = strcmp(a, b2) == 0;
        }
        if (seen)
            continue;
        if (idx++ != k)
            continue;
        keys[0] = '\0';
        for (j = i; j < n; j++) {
            if (!kmModeApplies(list[j].mode, nwp_display_mode(g_w)) || strcmp(list[j].app, list[i].app) != 0)
                continue;
            kmFormatStmts(&list[j], b2, sizeof(b2));
            if (strcmp(a, b2) != 0)
                continue;
            kmFormatKey(list[j].key, 1, one, sizeof(one));
            if (strlen(keys) + strlen(one) + 3 >= kc)
                break;
            if (keys[0])
                strcat(keys, ", ");
            strcat(keys, one);
        }
        kmDescribe(&list[i], what, wc);
        return 1;
    }
    return 0;
}

static void helpDraw(struct window *w, int x, int y, int cols, int rows, int active)
{
    char line[200];
    char keys[96];
    char what[120];
    int row = y + 1;
    int k;
    int total = 0;

    (void)w;
    (void)active;
    while (helpLine(total, keys, sizeof(keys), what, sizeof(what)))
        total++;
    if (g_helpTop > total - 1)
        g_helpTop = total > 0 ? total - 1 : 0;
    if (g_helpTop < 0)
        g_helpTop = 0;
    screenText(x + 1, row++, "Welcome to Nyxis OS.", ROLE_WINDOW);
    snprintf(line, sizeof(line), "Change shortcuts: %s   Scroll: Up/Down", keyLabel(ACT_KEYS));
    screenText(x + 1, row++, line, ROLE_BORDER);
    row++;
    for (k = g_helpTop; row < y + rows - 1 && helpLine(k, keys, sizeof(keys), what, sizeof(what)); k++) {
        snprintf(line, sizeof(line), "%-26s %s", keys, what);
        if ((int)strlen(line) > cols - 2)
            line[cols > 2 ? cols - 2 : 0] = '\0';
        screenText(x + 1, row++, line, ROLE_WINDOW);
    }
    snprintf(line, sizeof(line), "%s@%s  %d/%d", g_user, g_host, total ? g_helpTop + 1 : 0, total);
    if ((int)strlen(line) > cols - 2)
        line[cols > 2 ? cols - 2 : 0] = '\0';
    screenText(x + 1, y + rows - 1, line, ROLE_SELECTION);
}

static int helpKey(struct window *w, int key)
{
    (void)w;
    switch (KEY_BASE(key)) {
    case KEY_UP:   g_helpTop--; return 1;
    case KEY_DOWN: g_helpTop++; return 1;
    case KEY_PGUP: g_helpTop -= 10; return 1;
    case KEY_PGDN: g_helpTop += 10; return 1;
    case KEY_HOME: g_helpTop = 0; return 1;
    default: return 0;
    }
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    struct window win;
    char err[160];
    int help = argc > 1 && strcmp(argv[1], "-h") == 0;
    int inhibited = 0;
    struct nx64_identity me;

    if (argc > 2 || (argc == 2 && !help)) {
        fputs("usage: nkeys [-h]   (-h: show the shortcuts)\n", stderr);
        return 2;
    }
    if (kmLoad(err, sizeof(err)) < 0)
        snprintf(g_k.msg, sizeof(g_k.msg), "%s", err);
    if (nx64_whoami(&me) == 0)
        snprintf(g_user, sizeof(g_user), "%s", me.name);
    if (nx64_info("hostname", g_host, sizeof(g_host)) <= 0)
        strcpy(g_host, "nyxis");
    if (help)
        g_w = nwp_open("nhelp", "help", 72, 0, NWP_STATE_NORMAL);
    else
        g_w = nwp_open("nkeys", "shortcuts", 0, 0, NWP_STATE_NORMAL);
    if (g_w == NULL) {
        fprintf(stderr, "nkeys: cannot open a window: %s (run it inside ntui)\n", strerror(errno));
        return 1;
    }
    reloadCopy();
    win.data = &g_k;
    win.wantClose = 0;
    while (!win.wantClose) {
        struct nwp_event ev;
        int r;

        nwp_clear(g_w, ROLE_WINDOW);
        if (help)
            helpDraw(&win, 0, 0, nwp_cols(g_w), nwp_rows(g_w), 1);
        else
            keysDraw(&win, 0, 0, nwp_cols(g_w), nwp_rows(g_w), 1);
        (void)nwp_commit(g_w);
        r = nwp_next_event(g_w, &ev, -1);
        if (r < 0)
            break;
        if (r == 0)
            continue;
        if (ev.type == NWP_EVENT_CLOSE) {
            if (help || !g_k.dirty)
                break;
            g_k.mode = K_CONFIRM_CLOSE;
        } else if (ev.type == NWP_EVENT_KEY) {
            if (help) {
                if (ev.key == 'q' || ev.key == NWP_KEY_ESC)
                    break;
                (void)helpKey(&win, ev.key);
            } else {
                (void)keysKey(&win, ev.key);
            }
        }
        if (!help && wantsAllKeys() != inhibited) {
            inhibited = wantsAllKeys();
            nwp_inhibit_shortcuts(g_w, inhibited);
        }
    }
    nwp_close(g_w);
    return 0;
}
