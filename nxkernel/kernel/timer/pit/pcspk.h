/*
 * pcspk.h - PC 스피커 (사각파 비프음, 화음) 드라이버
 *
 * 8254 PIT 의 채널 2 를 모드 3(사각파)으로 두고 포트 0x61 의 비트 0(게이트)과 비트 1(스피커 연결)을 켜면
 * 스피커에서 그 주파수의 사각파가 나온다. 채널 0(스케줄러 타이머, pit_base.c)과는 서로 다른 채널이다.
 *
 * 화음: 스피커는 한 번에 한 음만 낼 수 있다 (1비트 출력, 채널 하나). 그래서 여러 음은 칩튠처럼 **아르페지오**로
 * 낸다: 타이머 tick(100Hz = 10ms)마다 다음 음으로 바꾼다. 3음 화음이면 30ms 에 한 바퀴라 귀에는 화음으로 들린다.
 * 음은 절대 겹치지 않는다 (어느 순간이든 사각파 하나).
 *
 * 커널 안의 누구나 (드라이버, 경고음, 시스템 콜) 이 함수들을 쓴다. 사용자 프로그램은 시스템 콜
 * NxSpeakerTone(11) / NxSpeakerChord(12) 를 거친다 (nlibc 의 nx64api: nx64_tone / nx64_chord / nx64_beep).
 *
 * 소리는 하나뿐이다 (채널이 하나): 나중에 부른 쪽이 이긴다. 각 소리는 "주인" pid 를 기억해서
 * 그 프로세스가 끝나면 (Ctrl-C, kill 포함) 커널이 소리를 끈다 - 끝난 프로그램의 소리가 계속 울리지 않게.
 */
#ifndef PCSPK_H
#define PCSPK_H

#include "nyxis.h"

/* 낼 수 있는 주파수 범위 (Hz). 그 밖은 NinvalidArg. 0 은 "끄기" */
#define PCSPK_MIN_HZ  20U
#define PCSPK_MAX_HZ  20000U

/* 소리 길이의 상한 (ms). 0 = 다음 호출까지 계속 */
#define PCSPK_MAX_MS  60000U

/* 화음의 최대 음 수 */
#define PCSPK_MAX_VOICES 4U

/* 커널 자신이 낸 소리의 주인 pid (어떤 프로세스가 끝나도 꺼지지 않는다) */
#define PCSPK_OWNER_KERNEL 0U

/*
 * hz 의 사각파를 ms 동안 낸다 (기다리지 않는다: 바로 돌아오고 타이머가 ms 뒤에 끈다).
 *   hz == 0 이면 끈다. ms == 0 이면 다음 호출(또는 주인이 끝날 때)까지 계속 낸다.
 *   owner: 주인 pid (PCSPK_OWNER_KERNEL = 커널).
 * 인터럽트 문맥에서도 부를 수 있다.
 */
Nstatus pcspk_tone(u32 hz, u32 ms, u32 owner);

/*
 * 화음: hz[0..n) 을 아르페지오로 ms 동안 (기다리지 않는다). n 은 0..PCSPK_MAX_VOICES.
 *   0 Hz 항목은 건너뛴다 (모두 0 이거나 n == 0 이면 끄기). 남은 음이 하나면 pcspk_tone 과 같다.
 *   같은 화음을 다시 주면 아르페지오 순서를 처음으로 되돌리지 않는다 (프레임마다 늘려도 끊기지 않게).
 */
Nstatus pcspk_chord(const u32 *hz, u32 n, u32 ms, u32 owner);

/* 바로 끈다 */
void pcspk_off(void);

/*
 * hz 의 사각파를 ms 동안 내고 끝날 때까지 기다린다 (커널용 beep). 타이머가 아직 없거나 인터럽트가
 * 꺼져 있으면 소리를 짧게 낼 수 없으므로 아무것도 하지 않는다.
 */
void pcspk_beep(u32 hz, u32 ms);

/* 지금 나는 주파수 (꺼져 있으면 0. 화음이면 지금 차례의 음) */
u32 pcspk_current_hz(void);

/* 타이머 인터럽트에서 매 tick 부른다: 화음이면 다음 음으로, 정한 길이가 끝났으면 끈다 */
void pcspk_timer_tick(u64 tick);

/* 프로세스가 끝날 때 부른다: 그 프로세스가 낸 소리면 끈다 */
void pcspk_process_terminated(u32 pid);

#endif /* PCSPK_H */
