/*
 * inputs_base.c - PS/2 키보드 드라이버 (스캔코드 세트 1, IRQ1 인터럽트 방식)
 *
 * 키를 누르거나 뗄 때마다 IRQ1 이 오고, 핸들러가 스캔코드를 키 이벤트(코드, 누름/뗌, 수정자, 글자)로 바꾼다.
 *   - dev:/console 이 키보드를 가져갔으면(keyboard_claim) 이벤트를 큐에 넣는다 (그 프로세스가 NxRead 로 읽는다).
 *   - 아니면 누름만 콘솔 터미널로: 글자는 tty_input_char(), 편집 키는 tty_input_key().
 *     Ctrl+문자는 제어 문자(Ctrl-D = 0x04 등)가 된다.
 *
 * [수정 이력 요약]
 *  - (이전) get_scancode 의 cli/sti 짝 깨짐, 0xE0 확장 키 릴리즈 처리, 테이블 범위 검사, C89 호환.
 *  - 폴링 API 를 인터럽트 드라이버로 교체 (inputs_base.h 참고).
 *  - SysRq: Alt 와 SysRq(Alt+PrintScreen = 0x54, 또는 E0 37)를 누른 채로 글자 키 -> sysrq_key (panic.c).
 *    그 키는 어디로도 가지 않는다. Alt+SysRq+C = 강제 커널 패닉. dev:/console 이 열려 있어도 같다.
 *  - 키 이벤트와 Super(E0 5B/5C), 오른쪽 Alt(AltGr, E0 38), 오른쪽 Ctrl(E0 1D) 구분, E1(Pause) 시퀀스 건너뛰기.
 */

#include "nyxis.h"
#include "lowlevel.h"
#include "interrupt.h"
#include "drivers/pic/pic.h"

#include "console/inputs/inputs_base.h"
#include "console/inputs/tty.h"
#include "kernel/error_handling/panic.h"

#define KBD_DATA     0x60
#define KBD_STATUS   0x64
#define KBD_CMD      0x64
#define KBD_IRQ      1U

#define KBD_ST_OUT_FULL 0x01
#define KBD_ST_IN_FULL  0x02

#define SC_SYSRQ     0x54           /* Alt 를 누른 채 PrintScreen 을 누르면 키보드가 보내는 코드 */
#define SC_PRTSC     0x137          /* E0 37: Alt 없이 누른 PrintScreen */
#define SC_FAKE_LSHIFT 0x12A        /* E0 2A / E0 36: 일부 키가 앞뒤에 붙이는 가짜 Shift (무시) */
#define SC_FAKE_RSHIFT 0x136

#define KBD_QUEUE    128U           /* 이벤트 큐 (2의 거듭제곱) */

/* ========================== */
/* 키보드 상태 (파일 내부)      */
/* ========================== */

static u8 shift_l = 0;
static u8 shift_r = 0;
static u8 ctrl_l = 0;
static u8 ctrl_r = 0;
static u8 alt_l = 0;
static u8 alt_r = 0;
static u8 super_l = 0;
static u8 super_r = 0;
static u8 caps_lock = 0;
static u8 extended = 0;
static u8 skip_bytes = 0;           /* E1 시퀀스(Pause)의 남은 바이트 */
static u8 sysrq = 0;                /* SysRq(PrintScreen) 키를 누르고 있다 */

static volatile bool g_claimed = false;
static struct nx_keyevent g_queue[KBD_QUEUE];
static u32 g_q_head = 0;
static u32 g_q_count = 0;
static waitq_t g_kbd_waiters;

/* ========================== */
/* ASCII 테이블                */
/* ========================== */

static const utf8 normal[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',
    0,'*',0,' ',
    0,0,0,0,0,0,0,0,0,0,0,0,0,              /* 0x3A Caps, F1-F10, NumLock, ScrollLock */
    0,0,0,'-',0,0,0,'+'                     /* 0x47-0x4E 키패드 (방향/숫자 키는 글자가 아니다) */
};

static const utf8 shifted[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',
    0,'*',0,' ',
    0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,'-',0,0,0,'+'
};

/* ========================== */
/* 문자 변환                   */
/* ========================== */

static u16 mods_now(void)
{
    u16 m = 0;

    if (shift_l || shift_r) m |= NX_KMOD_SHIFT;
    if (ctrl_l || ctrl_r)   m |= NX_KMOD_CTRL;
    if (alt_l)              m |= NX_KMOD_ALT;
    if (alt_r)              m |= NX_KMOD_ALTGR;
    if (super_l || super_r) m |= NX_KMOD_SUPER;
    if (caps_lock)          m |= NX_KMOD_CAPS;
    return m;
}

/* Shift/Caps 를 반영한 글자 (Ctrl 은 반영하지 않는다). 글자가 아닌 키는 0 */
static utf8 key_char(u16 code)
{
    utf8 c;

    if (code == NX_KEY_KP_ENTER)
        return '\n';
    if (code == 0x135)
        return '/';                         /* 키패드 / */
    if (code >= 128)
        return 0;
    c = (shift_l || shift_r) ? shifted[code] : normal[code];
    if (caps_lock && c >= 'a' && c <= 'z')
        c = (utf8)(c - 32);
    else if (caps_lock && c >= 'A' && c <= 'Z')
        c = (utf8)(c + 32);
    return c;
}

/* 수정자 키면 상태를 바꾸고 true */
static bool update_modifier(u16 code, bool pressed)
{
    u8 v = pressed ? 1 : 0;

    switch (code) {
    case NX_KEY_LSHIFT: shift_l = v; return true;
    case NX_KEY_RSHIFT: shift_r = v; return true;
    case NX_KEY_LCTRL:  ctrl_l = v; return true;
    case NX_KEY_RCTRL:  ctrl_r = v; return true;
    case NX_KEY_LALT:   alt_l = v; return true;
    case NX_KEY_RALT:   alt_r = v; return true;
    case NX_KEY_LSUPER: super_l = v; return true;
    case NX_KEY_RSUPER: super_r = v; return true;
    case NX_KEY_CAPSLOCK:
        if (pressed)
            caps_lock ^= 1;
        return true;
    default:
        return false;
    }
}

/* ========================== */
/* 이벤트 큐 (dev:/console)    */
/* ========================== */

static void queue_push(u16 code, bool pressed, utf8 ch)
{
    struct nx_keyevent *e;

    if (g_q_count == KBD_QUEUE)
        return;                             /* 가득 찼다: 읽지 않는 쪽의 이벤트는 버린다 */
    e = &g_queue[(g_q_head + g_q_count) % KBD_QUEUE];
    e->code = code;
    e->mods = mods_now();
    e->pressed = pressed ? 1 : 0;
    e->ch = (u8)ch;
    e->reserved = 0;
    g_q_count++;
    (void)waitq_wake_all(&g_kbd_waiters);
}

void keyboard_claim(bool on)
{
    u64 flags = irq_save();

    g_claimed = on;
    g_q_head = 0;
    g_q_count = 0;
    irq_restore(flags);
}

u32 keyboard_take_events(struct nx_keyevent *out, u32 max)
{
    u64 flags = irq_save();
    u32 n = 0;

    while (n < max && g_q_count > 0) {
        out[n++] = g_queue[g_q_head];
        g_q_head = (g_q_head + 1U) % KBD_QUEUE;
        g_q_count--;
    }
    irq_restore(flags);
    return n;
}

bool keyboard_events_ready(void)
{
    return g_q_count > 0 ? true : false;
}

waitq_t *keyboard_waitq(void)
{
    return &g_kbd_waiters;
}

/* ========================== */
/* 콘솔 터미널로 (가져가지 않았을 때) */
/* ========================== */

static void to_console(u16 code, utf8 c)
{
    switch (code) {
    case NX_KEY_UP:     tty_input_key(TTY_KEY_UP); return;
    case NX_KEY_DOWN:   tty_input_key(TTY_KEY_DOWN); return;
    case NX_KEY_LEFT:   tty_input_key(TTY_KEY_LEFT); return;
    case NX_KEY_RIGHT:  tty_input_key(TTY_KEY_RIGHT); return;
    case NX_KEY_HOME:   tty_input_key(TTY_KEY_HOME); return;
    case NX_KEY_END:    tty_input_key(TTY_KEY_END); return;
    case NX_KEY_DELETE: tty_input_key(TTY_KEY_DELETE); return;
    case NX_KEY_PGUP:   tty_input_key(TTY_KEY_PGUP); return;
    case NX_KEY_PGDN:   tty_input_key(TTY_KEY_PGDN); return;
    default: break;
    }
    if (!c)
        return;
    /* Ctrl+문자 -> 제어 문자 (Ctrl-D = 0x04, Ctrl-U = 0x15 ...) */
    if ((ctrl_l || ctrl_r) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c = (utf8)(c & 0x1F);
    else if ((ctrl_l || ctrl_r) && c == '\\')
        c = (utf8)0x1C;                     /* Ctrl-\ (SIGQUIT) */
    tty_input_char((char)c);
}

/* 스캔코드 바이트 하나를 처리한다 */
static void handle_scancode(u8 sc, const struct trap_frame *frame)
{
    bool pressed;
    u16 code;

    if (skip_bytes) {
        skip_bytes--;
        return;
    }
    if (sc == 0xE1) {                       /* Pause: E1 1D 45 E1 9D C5 (뗌이 없다) */
        skip_bytes = 5;
        return;
    }
    if (sc == 0xE0) {
        extended = 1;
        return;
    }

    pressed = (sc & 0x80) ? false : true;
    code = (u16)((sc & 0x7F) | (extended ? 0x100 : 0));
    extended = 0;

    if (code == SC_FAKE_LSHIFT || code == SC_FAKE_RSHIFT)
        return;
    if (code == SC_SYSRQ || code == SC_PRTSC) {
        sysrq = pressed ? 1 : 0;
        /* Alt 없이 누른 PrintScreen 은 dev:/console 의 프로그램에게도 (스크린샷). Alt+SysRq 조합은 커널 것 */
        if (g_claimed && code == SC_PRTSC && !alt_l && !alt_r)
            queue_push(code, pressed, 0);
        return;
    }

    /* Alt+SysRq 를 누른 채 글자 키: 커널이 직접 처리한다 (sysrq_key 가 돌아오지 않을 수도 있다: c = 패닉) */
    if (pressed && (alt_l || alt_r) && sysrq && code < 128 && normal[code] >= 'a' && normal[code] <= 'z') {
        (void)sysrq_key((char)normal[code], frame);
        return;
    }

    if (update_modifier(code, pressed)) {
        if (g_claimed)
            queue_push(code, pressed, 0);
        return;
    }

    if (g_claimed) {
        queue_push(code, pressed, key_char(code));
        return;
    }
    if (pressed)
        to_console(code, key_char(code));
}

static void keyboard_irq_handler(struct trap_frame *frame)
{
    u32 guard = 0;

    /* 한 번의 IRQ 에 여러 바이트가 쌓여 있을 수 있다. 무한 루프 방지 상한 */
    while ((inb(KBD_STATUS) & KBD_ST_OUT_FULL) && guard++ < 16U)
        handle_scancode(inb(KBD_DATA), frame);
}

/* 컨트롤러 입력 버퍼가 빌 때까지 기다린다 (상한 있음). 성공하면 true */
static bool kbd_wait_writable(void)
{
    u32 i;

    for (i = 0; i < 100000U; i++) {
        if (!(inb(KBD_STATUS) & KBD_ST_IN_FULL))
            return true;
    }
    return false;
}

static bool kbd_wait_readable(void)
{
    u32 i;

    for (i = 0; i < 100000U; i++) {
        if (inb(KBD_STATUS) & KBD_ST_OUT_FULL)
            return true;
    }
    return false;
}

void keyboard_init(void)
{
    u8 config;
    u32 i;

    waitq_init(&g_kbd_waiters);

    /* 컨트롤러가 없는 시스템(레거시 없는 실기 등)은 상태 포트가 0xFF 다 */
    if (inb(KBD_STATUS) == 0xFF)
        return;

    /* 펌웨어가 남긴 바이트를 비운다 */
    for (i = 0; i < 32U && (inb(KBD_STATUS) & KBD_ST_OUT_FULL); i++)
        (void)inb(KBD_DATA);

    /* 설정 바이트: bit0 = 키보드 IRQ1 허용, bit4 = 키보드 클럭 끔(0 이어야 함), bit6 = 세트1 변환 */
    if (!kbd_wait_writable())
        return;
    outb(KBD_CMD, 0x20);
    if (!kbd_wait_readable())
        return;
    config = inb(KBD_DATA);
    config = (u8)((config | 0x01U | 0x40U) & ~0x10U);
    if (!kbd_wait_writable())
        return;
    outb(KBD_CMD, 0x60);
    if (!kbd_wait_writable())
        return;
    outb(KBD_DATA, config);

    if (irq_register_handler((u8)KBD_IRQ, keyboard_irq_handler) != NSTATUS_OK)
        return;
    pic_clear_mask((u8)KBD_IRQ);
}
