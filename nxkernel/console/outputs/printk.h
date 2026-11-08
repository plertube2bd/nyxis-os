/*
 * printk.h - 커널 로그 출력
 *
 * 서식을 풀어 콘솔 출력 장치(console/outputs/conout.h)로 보낸다. 사용자 프로세스의 stdout/stderr 는
 * 여기를 거치지 않는다 (console/inputs/tty.h 의 tty_write).
 *
 * 지원 서식: %% %c %s %d %i %u %x %X %p %r  (길이 수정자: l, ll, z)
 *            폭/0 채움 (예: %08x, %016lx)
 *   %r : Nstatus 를 0x%08x 로 출력
 *   %p : 포인터를 0x%016lx 로 출력
 */
#ifndef PRINTK_H
#define PRINTK_H

#include "nyxis.h"

Nstatus printk(const char *format, ...);

/*
 * 색: 서식 문자열 안에서만 쓸 수 있다 (예: printk(KC_GREEN "ok" KC_RESET "\n")).
 * %s 로 넘긴 문자열 안의 ESC 등 제어 문자는 '?' 로 바뀐다 (외부 문자열의 이스케이프 주입 방지).
 * 해석 범위는 console/outputs/ansi.h 와 같다. 메시지마다 커널 쪽 색은 기본값에서 시작하고,
 * 메시지가 색을 바꿨다면 끝난 뒤 사용자 출력 쪽 색으로 되돌린다.
 */
#define KC_RESET   "\033[0m"
#define KC_BOLD    "\033[1m"
#define KC_RED     "\033[31m"
#define KC_GREEN   "\033[32m"
#define KC_YELLOW  "\033[33m"
#define KC_BLUE    "\033[34m"
#define KC_MAGENTA "\033[35m"
#define KC_CYAN    "\033[36m"
#define KC_GRAY    "\033[90m"

/* 태그 붙은 한 줄: "[  OK  ] " 초록, "[ WARN ] " 노랑, "[FAILED] " 빨강 (부팅 단계 결과 표시) */
Nstatus printk_ok(const char *format, ...);
Nstatus printk_warn(const char *format, ...);
Nstatus printk_err(const char *format, ...);

/*
 * 서식 없이 바이트열을 커널 로그에 그대로 출력한다 (NxKernelPrint 용).
 * 보안: 출력 가능한 ASCII 와 \n \r \t 만 통과시키고 나머지 바이트(특히 ESC 등 제어 문자)는 '?' 로 바꾼다.
 * 그렇지 않으면 사용자 프로그램이 시리얼 터미널로 이스케이프 시퀀스를 주입할 수 있다.
 */
void printk_write(const char *buf, usize len);

#endif /* PRINTK_H */
