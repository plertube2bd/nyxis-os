/*
 * pcspk.c - PC 스피커 (사각파 비프음, 아르페지오 화음) 드라이버. 설명은 pcspk.h
 *
 * 포트:
 *   0x43  PIT 명령. 0xB6 = 채널 2, lobyte/hibyte, 모드 3(사각파), 이진
 *   0x42  PIT 채널 2 데이터 (분주값 = PIT_BASE_FREQ / hz)
 *   0x61  키보드 컨트롤러 포트 B. 비트 0 = 채널 2 게이트, 비트 1 = 스피커 데이터 연결
 *         (다른 비트는 건드리지 않는다: 읽고, 두 비트만 바꾸고, 쓴다)
 *
 * 상태는 인터럽트를 끈 채로만 바꾼다 (타이머 인터럽트가 아르페지오를 돌린다).
 */

#include "nyxis.h"
#include "kernel/timer/pit/pit_base.h"
#include "kernel/timer/pit/pcspk.h"

#define PIT_CMD        0x43
#define PIT_CH2_DATA   0x42
#define SPK_PORT       0x61
#define SPK_BITS       0x03U
#define PIT_CH2_MODE3  0xB6U

static u32 g_voice[PCSPK_MAX_VOICES];   /* 화음의 음들 (0 이 아닌 것만, 앞에서부터) */
static u32 g_nvoices = 0;               /* 0 = 꺼짐 */
static u32 g_cur = 0;                   /* 지금 차례의 음 번호 */
static u32 g_hw_hz = 0;                 /* 하드웨어에 지금 걸린 주파수 */
static u32 g_owner = PCSPK_OWNER_KERNEL;
static u64 g_stop_tick = 0;             /* 0 = 끝을 정하지 않음 */

/* 인터럽트를 끈 상태에서 부른다. 같은 주파수면 분주기를 다시 쓰지 않는다 (위상이 끊기지 않게) */
static void hw_set(u32 hz)
{
    u8 v;

    if (hz == g_hw_hz)
        return;
    v = inb(SPK_PORT);
    g_hw_hz = hz;
    if (hz == 0) {
        outb(SPK_PORT, (u8)(v & ~SPK_BITS));
        return;
    }
    {
        u32 div = (u32)(PIT_BASE_FREQ / hz);

        if (div < 1U)
            div = 1U;
        if (div > 0xFFFFU)
            div = 0xFFFFU;
        outb(PIT_CMD, (u8)PIT_CH2_MODE3);
        outb(PIT_CH2_DATA, (u8)(div & 0xFFU));
        outb(PIT_CH2_DATA, (u8)((div >> 8) & 0xFFU));
    }
    if ((v & SPK_BITS) != SPK_BITS)
        outb(SPK_PORT, (u8)(v | SPK_BITS));
}

/* 인터럽트를 끈 상태에서 */
static void stop_locked(void)
{
    hw_set(0);
    g_nvoices = 0;
    g_cur = 0;
    g_stop_tick = 0;
    g_owner = PCSPK_OWNER_KERNEL;
}

Nstatus pcspk_chord(const u32 *hz, u32 n, u32 ms, u32 owner)
{
    u32 v[PCSPK_MAX_VOICES];
    u32 nv = 0;
    u32 i;
    u64 flags;
    u32 timer_hz;
    bool same;

    if (n > PCSPK_MAX_VOICES || ms > PCSPK_MAX_MS || (n > 0 && !hz))
        return NinvalidArg;
    for (i = 0; i < n; i++) {
        if (hz[i] == 0)
            continue;
        if (hz[i] < PCSPK_MIN_HZ || hz[i] > PCSPK_MAX_HZ)
            return NinvalidArg;
        v[nv++] = hz[i];
    }

    flags = irq_save();
    if (nv == 0) {
        stop_locked();
        irq_restore(flags);
        return NSTATUS_OK;
    }
    same = (nv == g_nvoices);
    for (i = 0; same && i < nv; i++)
        if (v[i] != g_voice[i])
            same = false;
    if (!same) {
        for (i = 0; i < nv; i++)
            g_voice[i] = v[i];
        g_nvoices = nv;
        g_cur = 0;
    }
    hw_set(g_voice[g_cur]);
    g_owner = owner;
    timer_hz = timer_get_hz();
    if (ms == 0 || timer_hz == 0)
        g_stop_tick = 0;
    else
        g_stop_tick = timer_get_tick() + ((u64)ms * timer_hz + 999UL) / 1000UL;   /* 올림 */
    irq_restore(flags);
    return NSTATUS_OK;
}

Nstatus pcspk_tone(u32 hz, u32 ms, u32 owner)
{
    return pcspk_chord(&hz, 1, ms, owner);
}

void pcspk_off(void)
{
    u64 flags = irq_save();

    stop_locked();
    irq_restore(flags);
}

void pcspk_beep(u32 hz, u32 ms)
{
    if (timer_get_hz() == 0 || !(read_rflags() & RFLAGS_IF))
        return;
    if (NSTATUS_IS_ERR(pcspk_tone(hz, 0, PCSPK_OWNER_KERNEL)))
        return;
    sleep_ms(ms);
    pcspk_off();
}

u32 pcspk_current_hz(void)
{
    return g_nvoices ? g_voice[g_cur] : 0;
}

/* 타이머 인터럽트 안 (인터럽트가 꺼져 있다) */
void pcspk_timer_tick(u64 tick)
{
    if (g_nvoices == 0)
        return;
    if (g_stop_tick != 0 && tick >= g_stop_tick) {
        stop_locked();
        return;
    }
    if (g_nvoices > 1) {
        g_cur = (g_cur + 1) % g_nvoices;
        hw_set(g_voice[g_cur]);
    }
}

void pcspk_process_terminated(u32 pid)
{
    u64 flags = irq_save();

    if (g_nvoices != 0 && pid != PCSPK_OWNER_KERNEL && g_owner == pid)
        stop_locked();
    irq_restore(flags);
}
