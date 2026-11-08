/*
 * rtc.c - CMOS 실시간 시계 (설계는 rtc.h 참고)
 */
#include "kernel/timer/rtc/rtc.h"
#include "kernel/timer/pit/pit_base.h"
#include "lowlevel.h"

#define CMOS_ADDR   0x70
#define CMOS_DATA   0x71
#define CMOS_NMI_OFF 0x80       /* 주소 포트의 7번 비트: NMI 를 막은 채로 읽는다 */

#define RTC_SEC     0x00
#define RTC_MIN     0x02
#define RTC_HOUR    0x04
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

#define STATUS_A_UIP    0x80    /* 갱신 중 (이때 읽으면 값이 섞일 수 있다) */
#define STATUS_B_24H    0x02
#define STATUS_B_BINARY 0x04

static bool g_rtc_valid;
static i64  g_boot_epoch_ns;   /* 부팅(단조 시계 0) 시점의 Unix 시간 */

static u8 cmos_read(u8 reg)
{
    outb(CMOS_ADDR, (u8)(CMOS_NMI_OFF | reg));
    return inb(CMOS_DATA);
}

struct rtc_time {
    u8 sec, min, hour, day, month, year;
};

static void read_raw(struct rtc_time *t)
{
    u32 spins = 0;

    while ((cmos_read(RTC_STATUS_A) & STATUS_A_UIP) && spins < 1000000U)
        spins++;
    t->sec = cmos_read(RTC_SEC);
    t->min = cmos_read(RTC_MIN);
    t->hour = cmos_read(RTC_HOUR);
    t->day = cmos_read(RTC_DAY);
    t->month = cmos_read(RTC_MONTH);
    t->year = cmos_read(RTC_YEAR);
}

static u8 bcd(u8 v)
{
    return (u8)((v & 0x0F) + (v >> 4) * 10);
}

/* 그레고리력 날짜 -> 1970-01-01 이후 일 수 (y >= 1970) */
static i64 days_from_civil(i64 y, i64 m, i64 d)
{
    i64 era;
    i64 yoe;
    i64 doy;
    i64 doe;

    y -= m <= 2;
    era = y / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static Nstatus rtc_read_epoch(i64 *out_sec)
{
    struct rtc_time a;
    struct rtc_time b;
    u8 status_b;
    u32 tries = 0;
    i64 hour;
    bool pm;

    /* 두 번 연속 같은 값이 나올 때까지 읽는다 (읽는 사이에 초가 넘어가는 경우) */
    read_raw(&b);
    do {
        a = b;
        read_raw(&b);
        tries++;
    } while ((a.sec != b.sec || a.min != b.min || a.hour != b.hour || a.day != b.day
              || a.month != b.month || a.year != b.year) && tries < 10);

    status_b = cmos_read(RTC_STATUS_B);
    pm = (b.hour & 0x80) != 0;
    b.hour &= 0x7F;
    if (!(status_b & STATUS_B_BINARY)) {
        b.sec = bcd(b.sec);
        b.min = bcd(b.min);
        b.hour = bcd(b.hour);
        b.day = bcd(b.day);
        b.month = bcd(b.month);
        b.year = bcd(b.year);
    }
    hour = b.hour;
    if (!(status_b & STATUS_B_24H)) {          /* 12시간제: 12 AM = 0시, 12 PM = 12시 */
        hour %= 12;
        if (pm)
            hour += 12;
    }
    if (b.sec > 59 || b.min > 59 || hour > 23 || b.day < 1 || b.day > 31 || b.month < 1 || b.month > 12
        || b.year > 99)
        return Nunsupported;                    /* RTC 가 없거나 값이 엉망 */

    /* 세기 레지스터는 펌웨어마다 위치가 달라서 쓰지 않는다: 2000~2099 년으로 본다 */
    *out_sec = days_from_civil(2000 + (i64)b.year, b.month, b.day) * 86400
             + hour * 3600 + (i64)b.min * 60 + (i64)b.sec;
    return NSTATUS_OK;
}

Nstatus rtc_realtime_ns(u64 *out_ns)
{
    if (!g_rtc_valid) {
        i64 sec;
        u64 flags = irq_save();
        Nstatus st = rtc_read_epoch(&sec);
        u64 mono = timer_get_ns();

        irq_restore(flags);
        if (NSTATUS_IS_ERR(st))
            return st;
        g_boot_epoch_ns = sec * 1000000000L - (i64)mono;
        g_rtc_valid = true;
    }
    *out_ns = (u64)(g_boot_epoch_ns + (i64)timer_get_ns());
    return NSTATUS_OK;
}
