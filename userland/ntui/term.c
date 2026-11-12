/*
 * term.c - 입력: 키 이벤트(화면 모드) 또는 터미널 바이트(터미널 모드)를 KEY_* + 수정자로
 *
 * 화면 모드 (screen.c 가 dev:/console 을 열었다): 커널의 키 이벤트(누름/뗌, 수정자)를 받는다.
 *   - 글자는 Shift/Caps 를 반영한 ASCII. Ctrl+글자는 제어 문자 (Ctrl+T = 0x14).
 *   - Alt/Super 는 KEY_MOD_ALT / KEY_MOD_SUPER 비트로. 특수 키에는 Shift/Ctrl 비트도 붙는다.
 *   - Super 를 눌렀다가 다른 키 없이 떼면 KEY_SUPER_TAP (메뉴).
 * 터미널 모드: 터미널을 raw 로 두고 바이트를 읽는다. VT 시퀀스(ESC [ A, ESC [ 1 ; 5 A ...)는 특수 키 + 수정자로,
 *   ESC + 글자는 Alt+글자로 묶는다. VMIN=0, VTIME=1: ESC 단독 키를 구분하려고 이어지는 바이트는 100ms 만 기다린다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <nx64.h>

#include "ntui.h"

static int g_tty = -1;                  /* 터미널 모드: 콘솔 fd (dev:/tty, 안 되면 stdin) */
static struct termios g_orig;           /* 되돌릴 원래 설정 */
static struct nx64_display *g_disp;     /* 화면 모드 */

#define PENDING_MAX 32
static int g_pending[PENDING_MAX];      /* 화면 모드: 한 번에 읽은 이벤트에서 나온 키 */
static int g_pendHead;
static int g_pendCount;
static int g_superAlone;                /* Super 를 누른 뒤 다른 키가 없었다 */
static int g_wantAltUp;                 /* 창 전환기: Alt 뗌을 KEY_ALT_UP 으로 */

void inputWantAltRelease(int on)
{
    g_wantAltUp = on;
}

/* raw 모드를 끄고 원래대로. 프로세스가 죽으면 커널도 cooked 로 되돌린다 */
static void ttyRestore(void)
{
    if (g_tty >= 0)
        tcsetattr(g_tty, TCSAFLUSH, &g_orig);
}

static int ttyOpenRaw(void)
{
    struct termios t;
    int fd;

    fd = open("dev:/tty", O_RDWR);
    if (fd < 0)
        fd = 0;
    if (tcgetattr(fd, &g_orig) < 0)
        goto fail;
    t = g_orig;
    cfmakeraw(&t);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSAFLUSH, &t) < 0)
        goto fail;
    if (fd > 0)
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);   /* 띄우는 명령에게 넘기지 않는다 */
    g_tty = fd;
    atexit(ttyRestore);
    return 0;

fail:
    if (fd > 0)
        close(fd);
    return -1;
}

int inputOpen(void)
{
    if (g_disp != NULL || g_tty >= 0)
        return 0;
    g_disp = screenDisplay();
    if (g_disp != NULL)
        return 0;
    return ttyOpenRaw();
}

int inputDisplayMode(void)
{
    return g_disp != NULL;
}

int ttyFd(void)
{
    return g_tty;
}

int inputFd(void)
{
    return g_disp != NULL ? g_disp->fd : g_tty;
}

/* ---------------- 화면 모드: 키 이벤트 ---------------- */

static void pushKey(int k)
{
    if (g_pendCount < PENDING_MAX) {
        g_pending[(g_pendHead + g_pendCount) % PENDING_MAX] = k;
        g_pendCount++;
    }
}

static int popKey(void)
{
    int k = g_pending[g_pendHead];

    g_pendHead = (g_pendHead + 1) % PENDING_MAX;
    g_pendCount--;
    return k;
}

static int specialKey(unsigned code)
{
    switch (code) {
    case NX64_KC_UP:     case 0x48: return KEY_UP;      /* 0x48.. : 키패드 (Num Lock 없음 = 방향키) */
    case NX64_KC_DOWN:   case 0x50: return KEY_DOWN;
    case NX64_KC_LEFT:   case 0x4B: return KEY_LEFT;
    case NX64_KC_RIGHT:  case 0x4D: return KEY_RIGHT;
    case NX64_KC_HOME:   case 0x47: return KEY_HOME;
    case NX64_KC_END:    case 0x4F: return KEY_END;
    case NX64_KC_PGUP:   case 0x49: return KEY_PGUP;
    case NX64_KC_PGDN:   case 0x51: return KEY_PGDN;
    case NX64_KC_INSERT: case 0x52: return KEY_INS;
    case NX64_KC_DELETE: case 0x53: return KEY_DEL;
    case NX64_KC_ESC:       return KEY_ESC;
    case NX64_KC_ENTER:
    case NX64_KC_KP_ENTER:  return '\r';
    case NX64_KC_TAB:       return '\t';
    case NX64_KC_BACKSPACE: return 0x7F;
    case NX64_KC_F11:       return KEY_F11;
    case NX64_KC_F12:       return KEY_F12;
    default:
        if (code >= NX64_KC_F1 && code <= NX64_KC_F10)
            return KEY_F1 + (int)(code - NX64_KC_F1);
        return 0;
    }
}

/* 이벤트 하나 -> 키 (없으면 0) */
static int eventKey(const struct nx64_keyevent *e)
{
    int mods = 0;
    int base;

    if (e->code == NX64_KC_LSUPER || e->code == NX64_KC_RSUPER) {
        if (e->pressed) {
            g_superAlone = 1;
            return 0;
        }
        if (g_superAlone) {
            g_superAlone = 0;
            return KEY_SUPER_TAP;
        }
        return 0;
    }
    if (!e->pressed) {
        if (g_wantAltUp && (e->code == NX64_KC_LALT || e->code == NX64_KC_RALT) &&
            !(e->mods & (NX64_KMOD_ALT | NX64_KMOD_ALTGR)))
            return KEY_ALT_UP;
        return 0;
    }
    if (e->code == NX64_KC_PRTSC) {
        g_superAlone = 0;
        return KEY_PRINT;
    }
    if (e->code == NX64_KC_MENU) {
        g_superAlone = 0;
        return KEY_MENUKEY | ((e->mods & NX64_KMOD_SHIFT) ? KEY_MOD_SHIFT : 0);
    }
    if (e->mods & NX64_KMOD_ALT)
        mods |= KEY_MOD_ALT;
    if (e->mods & NX64_KMOD_SUPER)
        mods |= KEY_MOD_SUPER;

    base = specialKey(e->code);
    if (base == 0 && e->ch == 0) {
        /* 수정자 키 자체 (Shift/Ctrl/Alt/Caps): Super 단독 누름은 아니게 된다 */
        if (e->code != 0x2A && e->code != 0x36)
            g_superAlone = 0;
        return 0;
    }
    g_superAlone = 0;
    if (base != 0) {
        if (e->mods & NX64_KMOD_SHIFT)
            mods |= KEY_MOD_SHIFT;
        if (e->mods & NX64_KMOD_CTRL)
            mods |= KEY_MOD_CTRL;
        return base | mods;
    }

    base = e->ch;
    if (e->mods & NX64_KMOD_CTRL) {
        if ((base >= 'a' && base <= 'z') || (base >= 'A' && base <= 'Z') ||
            base == '[' || base == '\\' || base == ']' || base == '@' || base == '^' || base == '_')
            base &= 0x1F;
        else if (base == ' ')
            base = 0;                       /* Ctrl-Space = NUL: 키 값 0 은 "없음" 이므로 Ctrl 비트로 */
        if (base == 0)
            return ' ' | KEY_MOD_CTRL | mods;
        if (base >= 0x20)
            mods |= KEY_MOD_CTRL;
        else if (e->mods & NX64_KMOD_SHIFT)
            mods |= KEY_MOD_SHIFT;      /* Ctrl+Shift+글자: 제어 문자 + Shift (Ctrl+Shift+T 등을 구분하려고) */
    }
    return base | mods;
}

static int readDisplayKeys(int ms)
{
    struct nx64_keyevent ev[16];
    int n;
    int i;

    n = nx64_display_keys(g_disp, ev, 16, ms);
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    for (i = 0; i < n; i++) {
        int k = eventKey(&ev[i]);

        if (k != 0)
            pushKey(k);
    }
    return n;
}

/* ---------------- 터미널 모드: 바이트 해석 ---------------- */

/* 100ms 안에 1 바이트. 시간 초과면 -1, 읽기 오류면 -2 */
static int readb(void)
{
    unsigned char c;
    ssize_t n = read(g_tty, &c, 1);

    if (n == 1)
        return c;
    return n == 0 ? -1 : -2;
}

/* xterm 수정자 번호 (1 + Shift 1 + Alt 2 + Ctrl 4) -> KEY_MOD_* */
static int xtermMods(int m)
{
    int r = 0;

    if (m < 2)
        return 0;
    m -= 1;
    if (m & 1) r |= KEY_MOD_SHIFT;
    if (m & 2) r |= KEY_MOD_ALT;
    if (m & 4) r |= KEY_MOD_CTRL;
    return r;
}

static int ttyKey(int ms)
{
    int c;
    int a;
    int p[2] = { 0, 0 };
    int np = 0;
    int tries = ms < 0 ? -1 : (ms + 99) / 100;

    fflush(stdout);
    for (;;) {
        c = readb();                    /* 바이트가 없으면 100ms 뒤 -1 */
        if (c != -1)
            break;
        if (tries >= 0 && --tries <= 0)
            return KEY_TIMEOUT;         /* ms == 0 도 한 번(100ms)은 기다린다 */
    }
    if (c < 0)
        return EOF;
    if (c == 0x08)
        return 0x7F;
    if (c != 27)
        return c;

    /* PS/2 는 시퀀스가 한꺼번에 들어오고, 시리얼은 바이트가 따로 온다: 이어지는 바이트는 100ms 만 기다린다 */
    if ((a = readb()) < 0)
        return KEY_ESC;
    if (a != '[' && a != 'O') {
        if (a >= 0x20 && a < 0x7F)
            return a | KEY_MOD_ALT;     /* ESC + 글자 = Alt+글자 */
        return KEY_ESC;
    }
    for (;;) {
        int b = readb();

        if (b < 0)
            return KEY_ESC;
        if (b >= '0' && b <= '9') {
            if (np == 0)
                np = 1;
            p[np - 1] = p[np - 1] * 10 + (b - '0');
            continue;
        }
        if (b == ';') {
            if (np == 0)
                np = 1;
            if (np < 2)
                np++;
            continue;
        }
        {
            int mods = np >= 2 ? xtermMods(p[1]) : 0;

            switch (b) {
            case 'A': return KEY_UP | mods;
            case 'B': return KEY_DOWN | mods;
            case 'C': return KEY_RIGHT | mods;
            case 'D': return KEY_LEFT | mods;
            case 'H': return KEY_HOME | mods;
            case 'F': return KEY_END | mods;
            case 'Z': return '\t' | KEY_MOD_SHIFT;
            case 'P': return KEY_F1 | mods;
            case 'Q': return KEY_F2 | mods;
            case 'R': return KEY_F3 | mods;
            case 'S': return KEY_F4 | mods;
            case '~':
                switch (p[0]) {
                case 1: case 7: return KEY_HOME | mods;
                case 4: case 8: return KEY_END | mods;
                case 2: return KEY_INS | mods;
                case 3: return KEY_DEL | mods;
                case 5: return KEY_PGUP | mods;
                case 6: return KEY_PGDN | mods;
                case 15: return KEY_F5 | mods;
                case 17: case 18: case 19: case 20: case 21:
                    return (KEY_F6 + (p[0] - 17)) | mods;
                case 23: case 24: return (KEY_F11 + (p[0] - 23)) | mods;
                }
                return KEY_ESC;
            default:
                return KEY_ESC;
            }
        }
    }
}

/* ---------------- 공통 ---------------- */

/*
 * 키 하나를 최대 ms 밀리초 기다린다 (ms < 0 이면 끝없이, 0 이면 이미 온 것만).
 * 시간이 지나면 KEY_TIMEOUT, 입력이 사라지면 EOF.
 */
int getchTimeout(int ms)
{
    if (inputOpen() < 0)
        return EOF;
    if (g_disp == NULL)
        return ttyKey(ms);

    for (;;) {
        int r;

        if (g_pendCount > 0)
            return popKey();
        r = readDisplayKeys(ms);
        if (r < 0)
            return EOF;
        if (g_pendCount > 0)
            return popKey();
        if (ms >= 0)
            return KEY_TIMEOUT;
    }
}

/* 화면 모드: 이미 읽어 둔 키가 남았는지 (이벤트 루프가 poll 없이 마저 꺼낸다) */
int inputPending(void)
{
    return g_pendCount > 0;
}

int getch(void)
{
    return getchTimeout(-1);
}
