#include "nyxis.h"
#include "lowlevel.h"

#include "console/inputs_base.h"

// ==========================
// 키보드 상태
// ==========================

static u8 shift_l = 0;
static u8 shift_r = 0;
static u8 caps_lock = 0;
static u8 extended = 0;

// ==========================
// ASCII 테이블
// ==========================

static utf8 normal[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',
};

static utf8 shifted[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',
};

// ==========================
// 스캔코드 읽기 (hlt 사용)
// ==========================

u8 get_scancode() {
    while (1) {
        cli();
        if (inb(0x64) & 1) {
            return inb(0x60);
            sti();
        }
        sti();
        hlt();
    }
}

// ==========================
// 문자 변환
// ==========================

utf8 translate(u8 sc) {
    if (sc >= 128) return 0;
    u8 shift = shift_l || shift_r;
    utf8 c = shift ? shifted[sc] : normal[sc];

    // Caps Lock 처리 (알파벳만)
    if (caps_lock && c >= 'a' && c <= 'z')
        c -= 32;
    else if (caps_lock && c >= 'A' && c <= 'Z')
        c += 32;
    return c;
}

// ==========================
// 메인 입력 함수
// ==========================

utf8 get_char() {
    while (1) {
        u8 sc = get_scancode();

        // ===== 확장키 prefix =====
        if (sc == 0xE0) {
            extended = 1;
            continue;
        }

        // ===== 릴리즈 =====
        if (sc & 0x80) {
            u8 key = sc & 0x7F;
            if (!extended) {
                // TODO: extended 구현
                if (key == 42) shift_l = 0;
                if (key == 54) shift_r = 0;
            }
            extended = 0;
            continue;
        }

        // ===== 프레스 =====
        if (!extended) {
            // TODO: extended 구현
            if (sc == 42) { shift_l = 1; continue; }
            if (sc == 54) { shift_r = 1; continue; }
            if (sc == 58) { // Caps Lock
                caps_lock ^= 1;
                continue;
            }
        }

        // ===== 일반 키 =====
        utf8 c = translate(sc);
        extended = 0;
        if (c) return c;
    }
}
