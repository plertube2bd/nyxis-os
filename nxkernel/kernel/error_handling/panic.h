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

struct trap_frame;

/*
 * kernel_panic_simple 과 같고, 패닉 당시의 CPU 상태(frame: 인터럽트/예외가 끊은 곳)와 그때의 프로세스도 찍는다.
 * frame 이 nNULL 이면 kernel_panic_simple 과 같다.
 */
void kernel_panic_frame(
    const char *Message,
    Nstatus error,
    const struct trap_frame *frame
) __attribute__((noreturn));

/*
 * SysRq (Linux 의 Magic SysRq 처럼): 키보드 Alt+SysRq+<키>, 또는 시리얼 BREAK 다음 <키>.
 * 인터럽트 처리기 안에서 부른다. key 는 소문자 글자. 처리했으면 true (그 키는 tty 로 보내지 않는다)
 *   c   강제 커널 패닉 (패닉 화면을 보거나 패닉 경로를 시험할 때)
 *   h   도움말 (위 목록)
 */
bool sysrq_key(char key, const struct trap_frame *frame);

#endif /* PANIC_C */
