/*
 * rtc.h - CMOS 실시간 시계 (MC146818 호환, 포트 0x70/0x71)
 *
 * 부팅 후 처음 필요할 때 한 번 읽어 "부팅 시각"(Unix 시간, 초)을 정해 두고, 그 뒤로는
 * 단조 시계(timer_get_ns)를 더해 실시간을 만든다 — RTC 를 매번 읽지 않고, 시간이 거꾸로 가지 않는다.
 * RTC 는 UTC 로 맞춰져 있다고 가정한다 (QEMU 기본값, 대부분의 유닉스 계열과 같은 관례).
 */
#ifndef NYX_RTC_H
#define NYX_RTC_H

#include "types.h"

/* 1970-01-01 00:00:00 UTC 이후 나노초. RTC 를 읽을 수 없으면 Nunsupported */
Nstatus rtc_realtime_ns(u64 *out_ns);

#endif /* NYX_RTC_H */
