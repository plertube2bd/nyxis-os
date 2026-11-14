/*
 * main.c - ntui 진입점, 이벤트 루프, 전역 단축키
 *
 * ntui 는 로그인 셸(콘솔 세션)로 시작하면 화면과 키보드를 직접 가진다 (screen.c / term.c 의 "화면 모드").
 * 창 프로그램(nterm, nfiles, nclock, nkeys ...)은 따로 도는 프로세스이고 NWP 로 연결한다 (server.c, <nwp.h>).
 *
 * 이벤트 루프: poll 로 키 입력, 창들이 기다리는 fd(터미널의 pty 출력), 시간(시계)을 함께 기다린다.
 *
 * 단축키: 모두 설정 keys 한 파일에 있다 (keymap.c. 기본값 rootfs/app/ntui/keys = app:/ntui.run/keys, 사용자 값
 * roi:/<uid>/app/ntui/keys). 키마다 어디로 갈지 정한다: ntui 동작, 명령, 활성 창의 프로그램(pass/send), 터미널 작업에
 * 신호. 묶이지 않은 키는 활성 창의 프로그램에게, 묶이지 않은 Super 키는 버린다. 순서는 아래 handleKey.
 * 단축키 설정 창(keys.c, 기본 Super+I)에서 키를 녹음해 바꾸고 저장한다. 도움말(F1)은 지금 설정을 보인다.
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

#define POLL_MAX_FDS 32                 /* NX_POLL_MAX: 입력 + 채널 + 클라이언트 파이프 */

/* ---------------- 단축키 (keymap.c) ---------------- */

/* 동작 action 을 하는 단축키의 이름 ("Super+R"). 지금 모드에서 쓸 수 있는 첫 바인딩 (창 조건이 없는 것). 없으면 "" */
const char *shortcutLabel(int action)
{
    static char bufs[6][32];
    static int next;
    const struct km_binding *list;
    char *buf = bufs[next];
    int n;
    int i;

    next = (next + 1) % 6;              /* 한 문장에서 여러 번 불러도 되게 */
    buf[0] = '\0';
    list = kmList(&n);
    for (i = 0; i < n; i++) {
        const struct km_binding *b = &list[i];

        if (b->nstmt == 1 && b->app[0] == '\0' && kmModeApplies(b->mode, inputDisplayMode()) && kmStmtAction(&b->s[0]) == action) {
            kmFormatKey(b->key, 1, buf, sizeof(bufs[0]));
            break;
        }
    }
    return buf;
}

/* 잠깐 보이는 알림 (스크린샷 저장 등) */
static char g_toast[128];
static unsigned long g_toastUntil;

void ntuiToast(const char *msg)
{
    snprintf(g_toast, sizeof(g_toast), "%s", msg);
    g_toastUntil = nx64_uptime_ms() + 3000UL;
}

static void openTaskManager(void)
{
    int i = wmFind("processes");

    if (i >= 0)
        wmSetActive(i);
    else
        (void)launchCommand("ntop", LAUNCH_AUTOCLOSE, "processes", 90, 28);
}

/* 하나만 띄우는 앱: 이미 창이 있으면 그 창으로 */
static void openSingle(const char *app_id, const char *const *argv)
{
    if (!serverActivateApp(app_id))
        (void)launch(argv);
}

void helpOpen(void)
{
    static const char *const argv[] = { "nkeys", "-h", NULL };

    openSingle("nhelp", argv);
}

void runAction(int action)
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
        (void)launchTerminal(NULL);
        break;
    case ACT_FILES:
        (void)launchApp("nfiles");
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
        serverThemeChanged();           /* 창 프로그램들도 새 색으로 다시 그린다 */
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
    case ACT_KEYS: {
        static const char *const argv[] = { "nkeys", NULL };

        openSingle("nkeys", argv);
        break;
    }
    case ACT_RELOAD_KEYS: {
        char err[160];

        if (kmLoad(err, sizeof(err)) < 0)
            ntuiToast(err);
        else
            ntuiToast(kmFromUser() ? "shortcuts reloaded (your file)" : "shortcuts reloaded (defaults)");
        break;
    }
    case ACT_SCREENSHOT:
        if (screenShot(path, sizeof(path)) == 0)
            snprintf(msg, sizeof(msg), "screenshot saved: %s", path);
        else
            snprintf(msg, sizeof(msg), "screenshot failed");
        ntuiToast(msg);
        break;
    default:
        break;
    }
}

/* 바인딩의 문장들을 차례로 한다 (keymap.c 의 문장 설명) */
static void runBinding(const struct km_binding *b, int key)
{
    int i;

    for (i = 0; i < b->nstmt; i++) {
        const struct km_stmt *st = &b->s[i];
        int act = kmStmtAction(st);

        switch (st->op) {
        case KM_ACTION:
            if (act == ACT_WINDOW)
                wmActivateNth(st->arg - 1);
            else
                runAction(act);
            break;
        case KM_EXEC:
            if (act != ACT_NONE)
                runAction(act);           /* nrunner nterminal nfiles ntop: ntui 안의 것 */
            else
                (void)launchAuto(st->text, 0);
            break;
        case KM_PASS:
            (void)wmKey(key);
            break;
        case KM_SEND:
            (void)wmKey(kmRaw(st->arg));
            break;
        case KM_SIGNAL:
            /* 신호는 창의 프로그램이 보낸다 (터미널이면 자기 포그라운드 작업에) */
            if (!serverSignalActive(st->arg))
                ntuiToast("signal: no window program is active");
            break;
        default:
            break;
        }
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
    if (strcmp(v, "terminal") == 0) {
        (void)launchTerminal(NULL);
        return;
    }
    if (strcmp(v, "files") == 0) {
        (void)launchApp("nfiles");
        return;
    }
    helpOpen();
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

/*
 * 키 하나가 갈 곳을 정한다:
 *   1. 잠금 화면 (잠겨 있으면 모든 키)
 *   2. 활성 창이 단축키를 막았으면(NWP_REQ_INHIBIT: nkeys 의 키 녹음) 그 창
 *   3. 창 전환기 / 창 메뉴 (열려 있으면 전부)
 *   4. 실행 상자 / 메뉴 (열려 있으면 전부. 다만 메뉴를 여는 키와 잠금 키는 단축키로)
 *   5. 단축키 설정(keymap.c)에 바인딩이 있으면 그 문장들 (ntui 동작, 명령, pass/send 로 프로그램에게, 신호)
 *   6. 작업 표시줄 포커스 중이면 작업 표시줄
 *   7. 바인딩이 없는 Super 키는 버린다 (화면 모드: 데스크톱 것), 그 밖은 활성 창의 프로그램에게
 * 끝내야 하면(메뉴의 Log out) 1
 */
static int handleKey(int key)
{
    const struct km_binding *b;
    int first = ACT_NONE;
    int m;

    if (lockKey(key))
        return 0;
    if (serverInhibited()) {            /* 활성 창이 단축키 없이 모든 키를 원한다 (키 녹음 등) */
        (void)wmKey(key);
        return 0;
    }
    if (switcherKey(key))
        return 0;
    if (key == KEY_ALT_UP)
        return 0;
    b = kmLookup(key, serverActiveAppId(), inputDisplayMode());
    if (b != NULL)
        first = kmStmtAction(&b->s[0]);
    if (wmMenuIsOpen() && first != ACT_WINMENU) {
        (void)wmMenuKey(key);
        return 0;
    }
    if (first == ACT_MENU && !nrunnerIsOpen()) {
        runBinding(b, key);
        return 0;
    }
    if (first != ACT_LOCK && nrunnerKey(key))
        return 0;
    m = menuKey(key);
    if (m == MENU_QUIT)
        return 1;
    if (m != MENU_IGNORED && first != ACT_LOCK)
        return 0;

    if (b != NULL) {
        runBinding(b, key);
        return 0;
    }

    wmSyncTaskbar();
    if (taskbarKey(key)) {
        wmSetActive(taskbarActive());
        return 0;
    }
    if ((key & KEY_MOD_SUPER) || key == KEY_SUPER_TAP)
        return 0;                       /* 묶이지 않은 Super 키: 프로그램에게 보내지 않는다 */
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
    {
        char err[160];

        if (kmLoad(err, sizeof(err)) < 0)
            ntuiToast(err);             /* 그래도 기본값/내장 단축키로 돈다 */
    }
    if (screenInit() < 0) {
        fprintf(stderr, "ntui: screen init failed\n");
        return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onSigpipe;
    (void)sigaction(SIGPIPE, &sa, NULL);
    if (serverInit() < 0)
        ntuiToast("cannot create the window service (svc:/<user>/nwp): no window programs");

    openStartup();

    while (!quit) {
        int n;
        int r;

        int ns;

        launchReap();
        if (serverConsumeReload()) {
            char err[160];

            ntuiToast(kmLoad(err, sizeof(err)) < 0 ? err : "shortcuts reloaded");
            dirty = 1;
        }
        if (wmReap())
            dirty = 1;
        if (wmTick())
            dirty = 1;
        if (dirty) {
            wmSyncTaskbar();
            draw();                     /* 창의 크기/포커스가 바뀌었으면 여기서 클라이언트에게 알린다 */
            screenFlush();
            dirty = 0;
        }
        (void)serverFlush();

        fds[0].fd = inputFd();
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        ns = serverPollFds(fds + 1, POLL_MAX_FDS - 1);
        n = 1 + ns + wmPollFds(fds + 1 + ns, owner + 1 + ns, POLL_MAX_FDS - 1 - ns);

        r = poll(fds, (nfds_t)n, 500);
        if (r < 0)
            continue;                   /* 신호 (EINTR) */
        if (r == 0) {
            dirty = taskbarTick() || g_toast[0] != '\0' || lockIsActive();   /* 분이 바뀌었거나 알림이 끝났다 */
            continue;
        }
        if (ns > 0 && serverPollDone(fds + 1, ns))
            dirty = 1;
        if (n > 1 + ns && wmPollDone(fds + 1 + ns, owner + 1 + ns, n - 1 - ns))
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

    serverShutdown();                   /* 창 프로그램들에게 끝내라고 하고 연결을 닫는다 */
    wmCloseAll();
    screenEnd();
    return 0;
}
