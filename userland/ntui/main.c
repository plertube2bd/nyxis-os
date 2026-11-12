/*
 * main.c - ntui 진입점, 이벤트 루프, 전역 단축키
 *
 * ntui 는 로그인 셸(콘솔 세션)로 시작하면 화면과 키보드를 직접 가진다 (screen.c / term.c 의 "화면 모드").
 * 창 안의 터미널은 ntui 의 터미널 에뮬레이터이고 명령은 의사 터미널에서 돈다 (nterminal.c).
 *
 * 이벤트 루프: poll 로 키 입력, 창들이 기다리는 fd(터미널의 pty 출력), 시간(시계)을 함께 기다린다.
 *
 * 단축키 (어느 창에서나). 다른 운영체제에서 흔히 쓰는 것을 따랐다 (Windows / GNOME / KDE / 터미널 에뮬레이터).
 * 화면 모드는 Super(Windows 키)/Alt/Ctrl+Alt, 터미널 모드(Super 가 없다)는 Ctrl 키와 Alt(ESC 접두어)로. 전체 표는
 * 아래 g_shortcuts (도움말 창 = F1 이 같은 표를 보인다):
 *   메뉴          Super 눌렀다 떼기, Super+Q, Alt+F1            실행          Super+R, Alt+F2
 *   새 터미널     Super+Enter, Super+T, Ctrl+Alt+T              파일          Super+E
 *   창 닫기       Alt+F4 (창이 없으면 전원 메뉴), Super+W       잠금          Super+L, Ctrl+Alt+L
 *   창 전환       Alt+Tab / Alt+Shift+Tab (Alt 를 떼면 그 창), Super+Tab (작업 보기), Alt+Esc (바로 다음 창)
 *   최대화        Super+Up, Super+F, Alt+F10                   되돌리기/최소화  Super+Down
 *   절반 배치     Super+Left / Super+Right                      전체 화면     F11
 *   최소화        Super+H, Alt+F9                               창 메뉴       Alt+Space, 메뉴 키
 *   옮기기/크기   Alt+F7 / Alt+F8                               바탕 화면     Super+D (다시 누르면 되살림)
 *   모두 최소화   Super+M, 모두 되살리기 Super+Shift+M            n 번째 창     Super+1..9
 *   작업 관리자   Ctrl+Shift+Esc, Ctrl+Alt+Del (ntop)           스크린샷      PrintScreen (face:/screenshot-*.ppm)
 *   작업 표시줄   Super+B   테마  Super+G   도움말  F1, Super+/
 *   터미널 창 안  Ctrl+Shift+T / Ctrl+Shift+N 새 터미널, Ctrl+Shift+W 닫기, Shift+PgUp/PgDn 스크롤백
 * 그 밖의 키는 활성 창이 받는다. 화면 모드에서는 Ctrl 키가 모두 창의 프로그램에게 간다 (터미널의 Ctrl-C, kilo 의 Ctrl-S ...).
 * 입력 칸(실행 상자, 잠금 화면)은 lineedit.c 의 편집 키 (Ctrl+<- ->, Ctrl+Backspace, Ctrl-W/U/K ...).
 * ntui 를 끝내는 것은 메뉴의 Log out 뿐이다. ntui 는 시스템에 하나만 돈다 (이미 있으면 시작하지 않는다).
 *
 * 설정 (nx64_setting_*: 사용자 값 roi:/<uid>/app/ntui/<키> 가 기본값 app:/ntui.run/<키> 보다 먼저):
 *   theme    테마 이름 (theme.c, 테마 바꾸기로 바꾸면 사용자 값으로 저장)
 *   startup  시작할 때 열 창: help (기본) | terminal | files | none
 *   display  terminal 이면 화면을 가져오지 않고 지금 터미널 위에서 돈다
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <nx64.h>

#include "ntui.h"

#define POLL_MAX_FDS 16

/* ---------------- 단축키 ---------------- */

/* 도움말/메뉴에 보일 이름 (화면 모드 / 터미널 모드) 과 설명. 순서 = 도움말 순서 */
static const struct {
    int action;
    const char *disp;
    const char *tty;
    const char *what;
} g_shortcuts[] = {
    { ACT_MENU,         "Super",          "^Q",       "menu (apps, theme, log out)" },
    { ACT_TERMINAL,     "Super+Enter",    "^T",       "new terminal (also Ctrl+Alt+T)" },
    { ACT_FILES,        "Super+E",        "^E",       "files" },
    { ACT_RUN,          "Super+R",        "^R",       "run a command (also Alt+F2)" },
    { ACT_CLOSE,        "Alt+F4",         "^W",       "close window" },
    { ACT_SWITCH_NEXT,  "Alt+Tab",        "Alt+Tab",  "switch window" },
    { ACT_SWITCH_PREV,  "Alt+Shift+Tab",  "^P",       "switch back" },
    { ACT_TASKVIEW,     "Super+Tab",      "",         "window list" },
    { ACT_NEXT,         "Alt+Esc",        "^N",       "next window" },
    { ACT_MAXIMIZE,     "Super+Up",       "^F",       "maximize / restore" },
    { ACT_RESTORE_MIN,  "Super+Down",     "^Y",       "restore / minimize" },
    { ACT_SNAP_LEFT,    "Super+Left",     "",         "left half" },
    { ACT_SNAP_RIGHT,   "Super+Right",    "",         "right half" },
    { ACT_FULLSCREEN,   "F11",            "F11",      "full screen" },
    { ACT_WINMENU,      "Alt+Space",      "Alt+Space", "window menu" },
    { ACT_MOVE,         "Alt+F7",         "^O",       "move window (F8 resize)" },
    { ACT_DESKTOP,      "Super+D",        "",         "show desktop" },
    { ACT_MINIMIZE_ALL, "Super+M",        "",         "minimize all (Shift: restore)" },
    { ACT_LOCK,         "Super+L",        "",         "lock screen" },
    { ACT_TASKMGR,      "Ctrl+Shift+Esc", "",         "processes (ntop)" },
    { ACT_SCREENSHOT,   "PrintScreen",    "",         "screenshot to face:/" },
    { ACT_TASKBAR,      "Super+B",        "^B",       "taskbar" },
    { ACT_THEME,        "Super+G",        "^G",       "change theme" },
    { ACT_HELP,         "F1",             "F1",       "this help" },
};
#define SHORTCUT_COUNT ((int)(sizeof(g_shortcuts) / sizeof(g_shortcuts[0])))

const char *shortcutLabel(int action)
{
    int i;

    for (i = 0; i < SHORTCUT_COUNT; i++) {
        if (g_shortcuts[i].action == action)
            return inputDisplayMode() ? g_shortcuts[i].disp : g_shortcuts[i].tty;
    }
    return "";
}

/* 터미널 모드의 Ctrl 단축키 (Super 가 없다) */
static const struct {
    int key;
    int action;
} g_ttyKeys[] = {
    { KEY_CTRL('q'), ACT_MENU }, { KEY_CTRL('r'), ACT_RUN }, { KEY_CTRL('t'), ACT_TERMINAL },
    { KEY_CTRL('e'), ACT_FILES }, { KEY_CTRL('w'), ACT_CLOSE }, { KEY_CTRL('f'), ACT_MAXIMIZE },
    { KEY_CTRL('y'), ACT_MINIMIZE }, { KEY_CTRL('o'), ACT_MOVE }, { KEY_CTRL('n'), ACT_NEXT },
    { KEY_CTRL('p'), ACT_PREV }, { KEY_CTRL('b'), ACT_TASKBAR }, { KEY_CTRL('g'), ACT_THEME },
};

static int superAction(int base, int shift, int *arg)
{
    if (base >= 'A' && base <= 'Z')
        base += 'a' - 'A';
    if (base >= '1' && base <= '9') {
        *arg = base - '1';
        return ACT_COUNT;               /* n 번째 창 (handleKey 가 따로 처리) */
    }
    switch (base) {
    case 'q': return ACT_MENU;
    case 'r': return ACT_RUN;
    case 't': case '\r': return ACT_TERMINAL;
    case 'e': return ACT_FILES;
    case 'w': return ACT_CLOSE;
    case 'f': case KEY_UP: return ACT_MAXIMIZE;
    case KEY_DOWN: return ACT_RESTORE_MIN;
    case KEY_LEFT: return ACT_SNAP_LEFT;
    case KEY_RIGHT: return ACT_SNAP_RIGHT;
    case 'h': return ACT_MINIMIZE;
    case 'n': return ACT_NEXT;
    case 'p': return ACT_PREV;
    case '\t': return ACT_TASKVIEW;
    case 'd': return ACT_DESKTOP;
    case 'm': return shift ? ACT_RESTORE_ALL : ACT_MINIMIZE_ALL;
    case 'l': return ACT_LOCK;
    case 'b': return ACT_TASKBAR;
    case 'g': return ACT_THEME;
    case '/': case '?': return ACT_HELP;
    default: return ACT_NONE;
    }
}

/* 키 -> 전역 동작 (ACT_NONE 이면 창에 넘긴다). Super+숫자는 ACT_COUNT 와 *arg */
static int actionOf(int key, int *arg)
{
    int base = KEY_BASE(key);
    int mods = key & KEY_MODS;
    int shift = (mods & KEY_MOD_SHIFT) != 0;
    int ctrl = (mods & KEY_MOD_CTRL) != 0;
    int alt = (mods & KEY_MOD_ALT) != 0;
    int i;

    if (key == KEY_SUPER_TAP)
        return ACT_MENU;
    if (key == KEY_PRINT)
        return ACT_SCREENSHOT;
    if (base == KEY_MENUKEY)
        return ACT_WINMENU;
    if (mods & KEY_MOD_SUPER)
        return superAction(base, shift, arg);

    /* Ctrl+Alt+글자 (글자는 제어 문자로 온다) */
    if (alt && base == KEY_CTRL('t'))
        return ACT_TERMINAL;
    if (alt && base == KEY_CTRL('l'))
        return ACT_LOCK;
    if (alt && ctrl && base == KEY_DEL)
        return ACT_TASKMGR;
    if (ctrl && shift && base == KEY_ESC)
        return ACT_TASKMGR;

    if (alt && !ctrl) {
        switch (base) {
        case '\t':   return shift ? ACT_SWITCH_PREV : ACT_SWITCH_NEXT;
        case KEY_ESC: return shift ? ACT_PREV : ACT_NEXT;
        case ' ':    return ACT_WINMENU;
        case KEY_F1: return ACT_MENU;
        case KEY_F2: return ACT_RUN;
        case KEY_F4: return ACT_CLOSE;
        case KEY_F7: return ACT_MOVE;
        case KEY_F8: return ACT_RESIZE;
        case KEY_F9: return ACT_MINIMIZE;
        case KEY_F10: return ACT_MAXIMIZE;
        default: break;
        }
    }
    if (key == KEY_F1)
        return ACT_HELP;
    if (key == KEY_F11)
        return ACT_FULLSCREEN;

    /* 터미널 에뮬레이터의 관례: 터미널 창 안의 Ctrl+Shift+T/N (새 터미널), Ctrl+Shift+W (닫기) */
    if (inputDisplayMode() && shift && !alt && wmActive() != NULL &&
        strcmp(wmActive()->app->name, "nterminal") == 0) {
        if (base == KEY_CTRL('t') || base == KEY_CTRL('n'))
            return ACT_TERMINAL;
        if (base == KEY_CTRL('w'))
            return ACT_CLOSE;
    }

    if (inputDisplayMode())
        return ACT_NONE;                /* 화면 모드의 Ctrl 키는 프로그램 것 */
    for (i = 0; i < (int)(sizeof(g_ttyKeys) / sizeof(g_ttyKeys[0])); i++) {
        if (key == g_ttyKeys[i].key)
            return g_ttyKeys[i].action;
    }
    return ACT_NONE;
}

/* 잠깐 보이는 알림 (스크린샷 저장 등) */
static char g_toast[128];
static unsigned long g_toastUntil;

static void toast(const char *msg)
{
    snprintf(g_toast, sizeof(g_toast), "%s", msg);
    g_toastUntil = nx64_uptime_ms() + 3000UL;
}

static void openTaskManager(void)
{
    int i = wmFind("processes");

    if (i >= 0)
        wmSetActive(i);
    else if (nterminalRunEx("ntop", RUN_AUTOCLOSE, "processes", 90, 28) == NULL)
        toast("cannot open a window");
}

static void perform(int action)
{
    char path[128];
    char msg[160];

    switch (action) {
    case ACT_MENU:
        if (menuIsOpen())
            menuClose();
        else
            menuOpen();
        break;
    case ACT_RUN:
        nrunnerOpen();
        break;
    case ACT_TERMINAL:
        (void)nterminalOpen();
        break;
    case ACT_FILES:
        (void)nfilesOpen(NULL);
        break;
    case ACT_CLOSE:
        if (wmActive() != NULL)
            wmCloseActive();
        else
            menuOpenAt('p');            /* 창이 없으면 전원 메뉴 (Windows 의 바탕 화면 Alt+F4) */
        break;
    case ACT_MAXIMIZE:
        if (wmActive() != NULL)
            wmToggleMaximize(wmActive());
        break;
    case ACT_MINIMIZE:
        wmMinimizeActive();
        break;
    case ACT_RESTORE_MIN:
        wmRestoreOrMinimize();
        break;
    case ACT_SNAP_LEFT:
    case ACT_SNAP_RIGHT:
        wmSnap(action == ACT_SNAP_LEFT ? -1 : 1);
        break;
    case ACT_FULLSCREEN:
        wmToggleFullscreen();
        break;
    case ACT_MOVE:
    case ACT_RESIZE:
        wmStartMove(action == ACT_RESIZE);
        break;
    case ACT_WINMENU:
        wmMenuOpen();
        break;
    case ACT_SWITCH_NEXT:
    case ACT_SWITCH_PREV:
        if (inputDisplayMode()) {
            switcherOpen(action == ACT_SWITCH_NEXT ? 1 : -1, 1);
            break;
        }
        /* 터미널 모드: Alt 를 뗀 것을 알 수 없으니 바로 다음 창으로 */
        action = action == ACT_SWITCH_NEXT ? ACT_NEXT : ACT_PREV;
        /* fall through */
    case ACT_NEXT:
    case ACT_PREV:
        wmSyncTaskbar();
        taskbarCycle(action == ACT_NEXT ? 1 : -1);
        wmSetActive(taskbarActive());
        break;
    case ACT_TASKVIEW:
        switcherOpen(0, 0);
        break;
    case ACT_DESKTOP:
        wmShowDesktop();
        break;
    case ACT_MINIMIZE_ALL:
        wmMinimizeAll();
        break;
    case ACT_RESTORE_ALL:
        wmRestoreAll();
        break;
    case ACT_TASKBAR:
        wmSyncTaskbar();
        taskbarToggleFocus();
        break;
    case ACT_THEME:
        themeNext();
        (void)themeSave();
        screenInvalidate();             /* 가장자리도 다시 칠한다 */
        break;
    case ACT_HELP:
        helpOpen();
        break;
    case ACT_LOCK:
        lockStart();
        break;
    case ACT_TASKMGR:
        openTaskManager();
        break;
    case ACT_SCREENSHOT:
        if (screenShot(path, sizeof(path)) == 0)
            snprintf(msg, sizeof(msg), "screenshot saved: %s", path);
        else
            snprintf(msg, sizeof(msg), "screenshot failed");
        toast(msg);
        break;
    default:
        break;
    }
}

/* 설정 startup 의 창을 연다 (모르는 값이면 기본 help) */
static void openStartup(void)
{
    char v[16];

    if (nx64_setting_get("ntui", "startup", v, sizeof(v)) < 0)
        strcpy(v, "help");
    if (strcmp(v, "none") == 0)
        return;
    if (strcmp(v, "terminal") == 0 && nterminalOpen() != NULL)
        return;
    if (strcmp(v, "files") == 0 && nfilesOpen(NULL) != NULL)
        return;
    helpOpen();
}

/* ---------------- 도움말 창 ---------------- */

static void helpDraw(struct window *w, int x, int y, int cols, int rows, int active)
{
    char line[96];
    int i;
    int row = y + 1;

    (void)w;
    (void)active;
    screenText(x + 1, row++, "Welcome to Nyxis OS.", ROLE_WINDOW);
    row++;
    for (i = 0; i < SHORTCUT_COUNT && row < y + rows - 2; i++) {
        const char *k = inputDisplayMode() ? g_shortcuts[i].disp : g_shortcuts[i].tty;

        if (k[0] == '\0')
            continue;                   /* 이 모드에는 없는 단축키 */
        snprintf(line, sizeof(line), "%-15s %s", k, g_shortcuts[i].what);
        if ((int)strlen(line) > cols - 2)
            line[cols > 2 ? cols - 2 : 0] = '\0';
        screenText(x + 1, row++, line, ROLE_WINDOW);
    }
    if (inputDisplayMode() && row < y + rows - 1)
        screenText(x + 1, row++, "Shift+PgUp/PgDn  terminal scrollback", ROLE_BORDER);
    if (row < y + rows) {
        snprintf(line, sizeof(line), "%s@%s  theme %s", username, hostname, currentTheme->name);
        if ((int)strlen(line) > cols - 2)
            line[cols > 2 ? cols - 2 : 0] = '\0';
        screenText(x + 1, row, line, ROLE_SELECTION);
    }
}

static const struct app g_helpApp = { "help", helpDraw, NULL, NULL, NULL, NULL, NULL };

void helpOpen(void)
{
    int i = wmFind("help");

    if (i >= 0)
        wmSetActive(i);
    else
        (void)wmOpen("help", &g_helpApp, NULL, 50, SHORTCUT_COUNT + 8);
}

/* ---------------- 그리기 ---------------- */

static void draw(void)
{
    char line[160];

    if (lockIsActive()) {
        lockDraw();
        return;
    }
    screenClear(ROLE_DESKTOP);
    if (wmVisibleCount() == 0) {
        snprintf(line, sizeof(line), "Nyxis OS  -  %s terminal   %s files   %s run   %s menu   %s help",
                 shortcutLabel(ACT_TERMINAL), shortcutLabel(ACT_FILES), shortcutLabel(ACT_RUN),
                 shortcutLabel(ACT_MENU), shortcutLabel(ACT_HELP));
        screenText(2, 1, line, ROLE_DESKTOP);
    }
    wmDraw();
    if (!wmFullscreenActive())
        taskbarDraw();
    nrunnerDraw();
    menuDraw();
    wmMenuDraw();
    switcherDraw();
    if (g_toast[0] && nx64_uptime_ms() < g_toastUntil) {
        int x = (screenCols - (int)strlen(g_toast) - 2) / 2;

        screenFill(x < 0 ? 0 : x, 0, (int)strlen(g_toast) + 2, 1, ' ', ROLE_SELECTION);
        screenText((x < 0 ? 0 : x) + 1, 0, g_toast, ROLE_SELECTION);
    } else {
        g_toast[0] = '\0';
    }
}

/* ---------------- 키 ---------------- */

/* 처리 순서: 잠금 화면 -> 창 전환기 / 창 메뉴 (열려 있으면 전부) -> 메뉴 열고 닫기 -> 실행 상자 / 메뉴 ->
 * 전역 단축키 -> 작업 표시줄 -> 창. 끝내야 하면(메뉴의 Log out) 1 */
static int handleKey(int key)
{
    int arg = 0;
    int action;
    int m;

    if (lockKey(key))
        return 0;
    if (switcherKey(key))
        return 0;
    if (key == KEY_ALT_UP)
        return 0;
    action = actionOf(key, &arg);
    if (wmMenuIsOpen() && action != ACT_WINMENU) {
        (void)wmMenuKey(key);
        return 0;
    }
    if (action == ACT_MENU && !nrunnerIsOpen()) {
        perform(action);
        return 0;
    }
    if (action != ACT_LOCK && nrunnerKey(key))
        return 0;
    m = menuKey(key);
    if (m == MENU_QUIT)
        return 1;
    if (m != MENU_IGNORED && action != ACT_LOCK)
        return 0;

    if (action == ACT_COUNT) {          /* Super+숫자: 작업 표시줄의 n 번째 창 */
        wmActivateNth(arg);
        return 0;
    }
    if (action != ACT_NONE) {
        perform(action);
        return 0;
    }

    wmSyncTaskbar();
    if (taskbarKey(key)) {
        wmSetActive(taskbarActive());
        return 0;
    }
    (void)wmKey(key);
    return 0;
}

/* SIGPIPE: 끝난 명령에 쓰면 온다. 처리기(무시가 아닌)를 두면 띄우는 명령에게는 기본값으로 넘어간다 */
static void onSigpipe(int sig)
{
    (void)sig;
}

int main(void)
{
    struct sigaction sa;
    struct pollfd fds[POLL_MAX_FDS];
    int owner[POLL_MAX_FDS];
    int dirty = 1;
    int quit = 0;

    {
        /* ntui 는 하나만: 이미 있으면(다른 세션, 또는 ntui 의 터미널 창에서 다시 띄움) 화면을 건드리지 않고 끝낸다 */
        unsigned long other = nx64_find_process("ntui");

        if (other != 0) {
            fprintf(stderr, "ntui: already running (pid %lu)\n", other);
            return 1;
        }
    }
    if (init() < 0)
        return 1;
    themeLoad();
    if (screenInit() < 0) {
        fprintf(stderr, "ntui: screen init failed\n");
        return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onSigpipe;
    (void)sigaction(SIGPIPE, &sa, NULL);

    openStartup();

    while (!quit) {
        int n;
        int r;

        if (wmReap())
            dirty = 1;
        if (wmTick())
            dirty = 1;
        if (dirty) {
            wmSyncTaskbar();
            draw();
            screenFlush();
            dirty = 0;
        }

        fds[0].fd = inputFd();
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        n = 1 + wmPollFds(fds + 1, owner + 1, POLL_MAX_FDS - 1);

        r = poll(fds, (nfds_t)n, 1000);
        if (r < 0)
            continue;                   /* 신호 (EINTR) */
        if (r == 0) {
            dirty = taskbarTick() || g_toast[0] != '\0' || lockIsActive();   /* 분이 바뀌었거나 알림이 끝났다 */
            continue;
        }
        if (n > 1 && wmPollDone(fds + 1, owner + 1, n - 1))
            dirty = 1;
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            do {
                int key = getchTimeout(0);

                if (key == EOF) {
                    quit = 1;
                    break;
                }
                if (key == KEY_TIMEOUT)
                    break;
                dirty = 1;
                if (handleKey(key)) {
                    quit = 1;
                    break;
                }
            } while (inputPending());
        }
    }

    wmCloseAll();
    screenEnd();
    return 0;
}
