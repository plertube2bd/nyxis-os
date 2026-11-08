/*
 * sys/wait.h - 자식 프로세스 기다리기
 *
 * 상태 값은 POSIX 관례대로 만든다: 정상 종료면 (코드 & 0xFF) << 8,
 * NxProcessKill 로 끝났으면 SIGKILL, 사용자 모드 CPU 예외로 끝났으면 SIGSEGV 로 "신호에 의한 종료".
 * 프로세스 그룹이 없으므로 pid 0 과 -1 이하는 모두 "아무 자식" 이다.
 */
#ifndef _NL_SYS_WAIT_H
#define _NL_SYS_WAIT_H

#include <sys/types.h>

#define WNOHANG 1

#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)
#define WTERMSIG(s)    ((s) & 0x7F)
#define WIFEXITED(s)   (WTERMSIG(s) == 0)
#define WIFSIGNALED(s) (WTERMSIG(s) != 0 && WTERMSIG(s) != 0x7F)
#define WIFSTOPPED(s)  (((s) & 0xFF) == 0x7F)
#define WSTOPSIG(s)    WEXITSTATUS(s)

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

#endif /* _NL_SYS_WAIT_H */
