/*
 * inputs_base.c - PS/2 키보드 드라이버 (스캔코드 세트 1, IRQ1 인터럽트 방식)
 *
 * 키를 누를 때마다 IRQ1 이 오고, 핸들러가 스캔코드를 문자로 바꿔 tty_input_char() 로 넘긴다.
 * Shift/Caps Lock/Ctrl 상태를 추적한다. Ctrl+문자는 제어 문자(Ctrl-D = 0x04 등)가 된다.
 *
 * [수정 이력 요약]
 *  - (이전) get_scancode 의 cli/sti 짝 깨짐, 0xE0 확장 키 릴리즈 처리, 테이블 범위 검사, C89 호환.
 *  - 폴링 API 를 인터럽트 드라이버로 교체 (inputs_base.h 참고).
 */

#include "nyxis.h"
#include "lowlevel.h"
#include "interrupt.h"
#include "drivers/pic/pic.h"

#include "console/inputs/inputs_base.h"
#include "console/inputs/tty.h"

#define KBD_DATA     0x60
#define KBD_STATUS   0x64
#define KBD_CMD      0x64
#define KBD_IRQ      1U

#define KBD_ST_OUT_FULL 0x01
#define KBD_ST_IN_FULL  0x02

#define SC_LSHIFT    42
#define SC_RSHIFT    54
#define SC_CTRL      29
#define SC_CAPS      58

/* ========================== */
/* 키보드 상태 (파일 내부)      */
/* ========================== */

static u8 shift_l = 0;
static u8 shift_r = 0;
static u8 caps_lock = 0;
static u8 extended = 0;
static u8 ctrl = 0;

/* ========================== */
/* ASCII 테이블                */
/* ========================== */

static const utf8 normal[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',
    0,'*',0,' '
};

static const utf8 shifted[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',
    0,'*',0,' '
};

/* ========================== */
/* 문자 변환                   */
/* ========================== */

static utf8 translate(u8 sc)
{
    u8 shift;
    utf8 c;

    if (sc >= 128)
        return 0;

    shift = (u8)(shift_l || shift_r);
    c = shift ? shifted[sc] : normal[sc];

    /* Caps Lock 처리 (알파벳만) */
    if (caps_lock && c >= 'a' && c <= 'z')
        c = (utf8)(c - 32);
    else if (caps_lock && c >= 'A' && c <= 'Z')
        c = (utf8)(c + 32);

    /* Ctrl+문자 -> 제어 문자 (Ctrl-D = 0x04, Ctrl-U = 0x15 ...) */
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c = (utf8)(c & 0x1F);

    return c;
}

/* 스캔코드 하나를 처리한다. 문자가 만들어지면 0 이 아닌 값을 돌려준다. */
static utf8 handle_scancode(u8 sc)
{
    utf8 c;

    /* ===== 확장키 prefix ===== */
    if (sc == 0xE0) {
        extended = 1;
        return 0;
    }

    /* ===== 릴리즈 ===== */
    if (sc & 0x80) {
        u8 key = (u8)(sc & 0x7F);

        if (key == SC_CTRL)
            ctrl = 0;                 /* 왼쪽(일반)/오른쪽(확장) Ctrl 모두 */
        /* 확장키의 릴리즈는 shift 상태에 영향을 주지 않는다 */
        if (!extended) {
            if (key == SC_LSHIFT) shift_l = 0;
            if (key == SC_RSHIFT) shift_r = 0;
        }
        extended = 0;
        return 0;
    }

    /* ===== 프레스 ===== */
    if (sc == SC_CTRL) {
        ctrl = 1;
        extended = 0;
        return 0;
    }
    if (!extended) {
        if (sc == SC_LSHIFT) { shift_l = 1; return 0; }
        if (sc == SC_RSHIFT) { shift_r = 1; return 0; }
        if (sc == SC_CAPS) {
            caps_lock ^= 1;
            return 0;
        }
    }

    /* ===== 일반 키 (확장키는 아직 문자로 바꾸지 않는다: 방향키 등) ===== */
    c = extended ? 0 : translate(sc);
    extended = 0;
    return c;
}

static void keyboard_irq_handler(struct trap_frame *frame)
{
    u32 guard = 0;

    (void)frame;

    /* 한 번의 IRQ 에 여러 바이트가 쌓여 있을 수 있다. 무한 루프 방지 상한 */
    while ((inb(KBD_STATUS) & KBD_ST_OUT_FULL) && guard++ < 16U) {
        utf8 c = handle_scancode(inb(KBD_DATA));

        if (c)
            tty_input_char((char)c);
    }
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
