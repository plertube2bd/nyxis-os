/*
 * inputs_base.c - PS/2 키보드 입력 (스캔코드 세트 1, 폴링)
 *
 * [수정 이력 요약]
 *  - get_scancode: 데이터를 읽은 뒤(return 이후) 도달하지 못하는 sti() 때문에,
 *    스캔코드를 읽을 때마다 "인터럽트가 꺼진 채로 반환" 되던 버그 수정.
 *    (cli 로 임계구역을 열고 sti 로 닫는 짝이 깨짐)
 *  - 인터럽트 상태를 저장/복원(irq_save/irq_restore)하여 호출자의 상태를 바꾸지 않는다.
 *  - 0xE0 확장 키의 릴리즈 시 shift 상태가 잘못 초기화되지 않게 정리.
 *  - 테이블 크기(128)를 넘는 스캔코드 접근 방지 (translate 에서 sc >= 128 이면 0 반환).
 *  - C89 호환.
 */

#include "nyxis.h"
#include "lowlevel.h"

#include "console/inputs/inputs_base.h"

/* ========================== */
/* 키보드 상태 (파일 내부)      */
/* ========================== */

static u8 shift_l = 0;
static u8 shift_r = 0;
static u8 caps_lock = 0;
static u8 extended = 0;

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
/* 스캔코드 읽기 (hlt 사용)     */
/* ========================== */

u8 get_scancode(void)
{
    for (;;) {
        u64 flags = irq_save();

        if (inb(0x64) & 1) {
            u8 sc = inb(0x60);

            irq_restore(flags);
            return sc;
        }

        /* 데이터가 없으면 인터럽트를 켠 뒤 다음 인터럽트까지 대기.
         * sti 와 hlt 는 연속 실행되어야 인터럽트를 놓치지 않는다 (x86 의 sti 지연 보장). */
        __asm__ volatile ("sti\n\thlt" : : : "memory");
        irq_restore(flags);
    }
}

/* ========================== */
/* 문자 변환                   */
/* ========================== */

utf8 translate(u8 sc)
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

    return c;
}

/* ========================== */
/* 메인 입력 함수              */
/* ========================== */

utf8 get_char(void)
{
    for (;;) {
        u8 sc = get_scancode();
        utf8 c;

        /* ===== 확장키 prefix ===== */
        if (sc == 0xE0) {
            extended = 1;
            continue;
        }

        /* ===== 릴리즈 ===== */
        if (sc & 0x80) {
            u8 key = (u8)(sc & 0x7F);

            /* 확장키(예: 오른쪽 Ctrl)의 릴리즈는 shift 상태에 영향을 주지 않는다 */
            if (!extended) {
                if (key == 42) shift_l = 0;
                if (key == 54) shift_r = 0;
            }
            extended = 0;
            continue;
        }

        /* ===== 프레스 ===== */
        if (!extended) {
            if (sc == 42) { shift_l = 1; continue; }
            if (sc == 54) { shift_r = 1; continue; }
            if (sc == 58) { /* Caps Lock */
                caps_lock ^= 1;
                continue;
            }
        }

        /* ===== 일반 키 ===== */
        c = extended ? 0 : translate(sc);
        extended = 0;
        if (c)
            return c;
    }
}
