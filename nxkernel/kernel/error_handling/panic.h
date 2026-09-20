/*
 * panic.h - 커널 패닉
 */
#ifndef PANIC_C
#define PANIC_C

#include "nyxis.h"

/*
 * 인터럽트를 끄고 메시지를 출력한 뒤 영구 정지한다. 절대 반환하지 않는다.
 * Message 는 반드시 서식 문자열이 아닌 일반 문자열로 취급되어 출력된다.
 */
void kernel_panic_simple(
    const char *Message,
    Nstatus error
) __attribute__((noreturn));

#endif /* PANIC_C */
